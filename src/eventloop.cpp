// ============================================================
// EventLoop：epoll ET 事件循环（网络层核心，每个线程一个）
// 三个自有 fd：
// - epfd：epoll 实例（所有 Channel 注册于此）
// - wakeupFd：eventfd，跨线程唤醒（stop/postToLoop/addPendingFlush）
// - timerFd：timerfd，固定 100ms 小周期扫描（handleTimer 按各定时器
//   next 判定到期；周期触发避免一次性 arm 的事件丢失）
// 事件分发的 UAF 防护：同批事件中前面的回调可能已销毁后续 channel，
// 故用 fd 作键查 channels_ 活跃表再分发，不直接解引用事件指针。
// 定时器：map<id, entry> 仅 loop 线程修改；cancelTimer 跨线程时投递
// 到 loop 执行。instanceId 全局编号防跨 loop 误取消。
// 线程身份 threadId_：由 EventLoopThread 在锁内 publishThisThread 发布
// （保证与 startLoop 条件等待构成 happens-before，TSan 竞争修复）。
// ============================================================
#include "eventloop.hpp"

#include "channel.hpp"
#include "connection.hpp"
#include "log.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>

namespace im {

namespace {
// 定时器基础扫描周期：100ms
constexpr long kTimerTickUs = 100000L; // 微妙
constexpr uint64_t kMaxTimerExp = 16384; // 一次最多处理的定时器触发数
}

std::atomic<uint64_t> EventLoop::s_instances_{0};

EventLoop::EventLoop() {
  instanceId_ = s_instances_.fetch_add(1, std::memory_order_relaxed);
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ == -1) {
    LOG_E("[eventloop] epoll_create1 失败: " << std::strerror(errno));
    ::exit(1);
  }
  wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeupFd_ == -1) {
    LOG_E("[eventloop] eventfd 失败: " << std::strerror(errno));
    ::exit(1);
  }
  wakeupChannel_ = std::make_unique<Channel>(wakeupFd_);
  wakeupChannel_->setReadCallback([this] {
    uint64_t one;
    // 清空 eventfd 计数，避免无限唤醒
    while (::read(wakeupFd_, &one, sizeof one) > 0) {}
  });
  wakeupChannel_->enableReading(this);

  timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerFd_ == -1) {
    LOG_E("[eventloop] timerfd_create 失败: " << std::strerror(errno));
    ::exit(1);
  }
  // 固定 100ms 小周期重复触发，handleTimer 里按各定时器的 next 判定到期。
  // 用周期触发可避免一次性 timerfd 重新 arm 导致的事件丢失问题。
  struct itimerspec its{};
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = kTimerTickUs * 1000L;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = kTimerTickUs * 1000L;
  ::timerfd_settime(timerFd_, 0, &its, nullptr);

  timerChannel_ = std::make_unique<Channel>(timerFd_);
  timerChannel_->setReadCallback([this] {
    uint64_t exp;
    // 已到期的定时器由 timerfd 触发，清空计数
    while (::read(timerFd_, &exp, sizeof exp) > 0) {}
    handleTimer();
  });
  timerChannel_->enableReading(this);
}

EventLoop::~EventLoop() {
  ::close(epfd_);
  ::close(wakeupFd_);
  if (timerFd_ >= 0) ::close(timerFd_);
}

void EventLoop::run() {
  // 线程身份由 EventLoopThread 在锁内经 publishThisThread() 发布（勿在此赋值：
  // 此处写入与主线程 isInLoopThread 的读缺乏同步边，TSan 报数据竞争）
  running_ = true;
  std::vector<struct epoll_event> events(256);
  while (running_) {
    int n = ::epoll_wait(epfd_, events.data(),
              static_cast<int>(events.size()), -1);
    if (n == -1) {
      if (errno == EINTR) continue;
      LOG_E("[eventloop] epoll_wait 失败: " << std::strerror(errno));
      break;
    }
    for (int i = 0; i < n; ++i) {
      // 同一批事件中，前面的回调可能已移除并销毁后续 channel（UAF 风险）。
      // 因此用 fd 作键查活跃表再分发，绝不直接解引用事件携带的指针。
      auto it = channels_.find(events[i].data.fd);
      if (it == channels_.end()) continue;
      it->second->handleEvent(events[i].events);
    }
    handlePending();
  }
}

void EventLoop::stop() {
  running_ = false;
  wakeup();
}

bool EventLoop::isInLoopThread() const {
  return threadId_ == std::this_thread::get_id();
}

void EventLoop::runInLoop(std::function<void()> task) {
  if (isInLoopThread()) {
    task();
  } else {
    postToLoop(std::move(task));
  }
}

void EventLoop::update(Channel* ch) {
  struct epoll_event ev{};
  ev.data.fd = ch->fd();
  ev.events = EPOLLET; // 边沿触发
  if (ch->isReading()) ev.events |= EPOLLIN | EPOLLRDHUP;
  if (ch->isWriting()) ev.events |= EPOLLOUT;
  if (channels_.count(ch->fd())) {
    ::epoll_ctl(epfd_, EPOLL_CTL_MOD, ch->fd(), &ev);
  } else {
    channels_[ch->fd()] = ch;
    ::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->fd(), &ev);
  }
}

void EventLoop::remove(Channel* ch) {
  channels_.erase(ch->fd());
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, ch->fd(), nullptr);
}

void EventLoop::wakeup() {
  uint64_t one = 1;
  // ET 下写入 eventfd 唤醒 loop；忽略 EAGAIN（已挂起一次唤醒）
  ssize_t r = ::write(wakeupFd_, &one, sizeof one);
  (void)r;
}

void EventLoop::postToLoop(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(taskMutex_);
    tasks_.push_back(std::move(task));
  }
  wakeup();
}

void EventLoop::addPendingFlush(std::shared_ptr<Connection> conn) {
  {
    std::lock_guard<std::mutex> lock(flushMutex_);
    pendingFlush_.push_back(std::move(conn));
  }
  wakeup();
}

void EventLoop::handlePending() {
  std::vector<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(taskMutex_);
    tasks.swap(tasks_);
  }
  for (auto& t : tasks) t();

  std::vector<std::shared_ptr<Connection>> flush;
  {
    std::lock_guard<std::mutex> lock(flushMutex_);
    flush.swap(pendingFlush_);
  }
  for (auto& conn : flush) conn->flushWrite();
}

EventLoop::TimerId EventLoop::addTimer(std::function<void()> cb,
                                       std::chrono::milliseconds interval) {
  TimerId id = nextTimerId_++;
  auto& e = timers_[id];
  e.cb = std::move(cb);
  e.interval = interval;
  e.active = true;
  e.next = std::chrono::steady_clock::now() + interval;
  scheduleNextTimer();
  return id;
}

void EventLoop::cancelTimer(TimerId id) {
  if (isInLoopThread()) {
    destroyTimer(id);
  } else {
    runInLoop([id, this] { destroyTimer(id); });
  }
}

void EventLoop::destroyTimer(TimerId id) {
  auto it = timers_.find(id);
  if (it != timers_.end()) {
    timers_.erase(it);
  }
  scheduleNextTimer();
}

void EventLoop::scheduleNextTimer() {
  // timerfd 已是固定 100ms 周期触发，无需逐次重 arm。
  // 若无任何定时器，则将周期拉长为最大间隔以节能。
  struct itimerspec its{};
  if (timers_.empty()) {
    its.it_value.tv_sec = 3600;
    its.it_interval.tv_sec = 3600;
  } else {
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = kTimerTickUs * 1000L;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = kTimerTickUs * 1000L;
  }
  ::timerfd_settime(timerFd_, 0, &its, nullptr);
}

void EventLoop::handleTimer() {
  auto now = std::chrono::steady_clock::now();

  // 收集所有已到期的定时器
  std::vector<uint64_t> due;
  for (auto& [id, e] : timers_) {
    if (e.active && e.next <= now) due.push_back(id);
  }

  for (auto id : due) {
    auto it = timers_.find(id);
    if (it == timers_.end() || !it->second.active) continue;

    auto cb = it->second.cb;
    if (it->second.interval.count() > 0) {
      // 周期定时器：以当前时间为基准安排下次触发
      it->second.next = now + it->second.interval;
      if (it->second.next <= now) it->second.next = now + std::chrono::milliseconds(1);
    } else {
      it->second.active = false;
      timers_.erase(it); // 一次性
    }
    cb();
  }
}

} // namespace im