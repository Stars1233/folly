/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/ThreadLocal.h>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#endif

#include <sys/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <boost/thread/barrier.hpp>
#include <glog/logging.h>

#include <folly/Memory.h>
#include <folly/experimental/io/FsUtil.h>
#include <folly/lang/Keep.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Unistd.h>
#include <folly/synchronization/Baton.h>
#include <folly/system/ThreadId.h>
#include <folly/testing/TestUtil.h>

using namespace folly;

extern "C" FOLLY_KEEP int* check_thread_local_get(ThreadLocal<int>& o) {
  return o.get();
}

extern "C" FOLLY_KEEP int* check_thread_local_get_existing(
    ThreadLocal<int>& o) {
  return o.get_existing();
}

template <typename>
struct static_meta_of;

template <template <typename...> class X, typename A0, typename... A>
struct static_meta_of<X<A0, A...>> {
  using type = folly::threadlocal_detail::StaticMeta<A...>;
};

struct Widget {
  static int totalVal_;
  static int totalMade_;
  int val_;
  Widget() : val_(0) { totalMade_++; }
  ~Widget() { totalVal_ += val_; }

  static void customDeleter(Widget* w, TLPDestructionMode mode) {
    totalVal_ += (mode == TLPDestructionMode::ALL_THREADS) ? 1000 : 1;
    delete w;
  }
};
int Widget::totalVal_ = 0;
int Widget::totalMade_ = 0;

struct MultiWidget {
  int val_{0};
  MultiWidget() = default;
  ~MultiWidget() {
    // force a reallocation in the destructor by
    // allocating more than elementsCapacity

    using TL = ThreadLocal<size_t>;
    using TLMeta = static_meta_of<TL>::type;
    auto const numElements = TLMeta::instance().elementsCapacity() + 1;
    std::vector<ThreadLocal<size_t>> elems(numElements);
    for (auto& t : elems) {
      *t += 1;
    }
  }
};

TEST(ThreadLocalPtr, BasicDestructor) {
  Widget::totalVal_ = 0;
  ThreadLocalPtr<Widget> w;
  std::thread([&w]() {
    w.reset(new Widget());
    w.get()->val_ += 10;
  }).join();
  EXPECT_EQ(10, Widget::totalVal_);
}

TEST(ThreadLocalPtr, CustomDeleter1) {
  Widget::totalVal_ = 0;
  {
    ThreadLocalPtr<Widget> w;
    std::thread([&w]() {
      w.reset(new Widget(), Widget::customDeleter);
      w.get()->val_ += 10;
    }).join();
    EXPECT_EQ(11, Widget::totalVal_);
  }
  EXPECT_EQ(11, Widget::totalVal_);
}

TEST(ThreadLocalPtr, CustomDeleterOwnershipTransfer) {
  Widget::totalVal_ = 0;
  {
    ThreadLocalPtr<Widget> w;
    auto deleter = [](Widget* ptr) {
      Widget::customDeleter(ptr, TLPDestructionMode::THIS_THREAD);
    };
    std::unique_ptr<Widget, decltype(deleter)> source(new Widget(), deleter);
    std::thread([&w, &source]() {
      w.reset(std::move(source));
      w.get()->val_ += 10;
    }).join();
    EXPECT_EQ(11, Widget::totalVal_);
  }
  EXPECT_EQ(11, Widget::totalVal_);
}

TEST(ThreadLocalPtr, DefaultDeleterOwnershipTransfer) {
  Widget::totalVal_ = 0;
  {
    ThreadLocalPtr<Widget> w;
    auto source = std::make_unique<Widget>();
    std::thread([&w, &source]() {
      w.reset(std::move(source));
      w.get()->val_ += 10;
    }).join();
    EXPECT_EQ(10, Widget::totalVal_);
  }
  EXPECT_EQ(10, Widget::totalVal_);
}

TEST(ThreadLocalPtr, resetNull) {
  ThreadLocalPtr<int> tl;
  EXPECT_FALSE(tl);
  tl.reset(new int(4));
  EXPECT_TRUE(static_cast<bool>(tl));
  EXPECT_EQ(*tl.get(), 4);
  tl.reset();
  EXPECT_FALSE(tl);
}

TEST(ThreadLocalPtr, TestRelease) {
  Widget::totalVal_ = 0;
  ThreadLocalPtr<Widget> w;
  std::unique_ptr<Widget> wPtr;
  std::thread([&w, &wPtr]() {
    w.reset(new Widget());
    w.get()->val_ += 10;

    wPtr.reset(w.release());
  }).join();
  EXPECT_EQ(0, Widget::totalVal_);
  wPtr.reset();
  EXPECT_EQ(10, Widget::totalVal_);
}

TEST(ThreadLocalPtr, CreateOnThreadExit) {
  Widget::totalVal_ = 0;
  ThreadLocal<Widget> w;
  ThreadLocalPtr<int> tl;

  std::thread([&] {
    tl.reset(new int(1), [&](int* ptr, TLPDestructionMode /* mode */) {
      delete ptr;
      // This test ensures Widgets allocated here are not leaked.
      ++w.get()->val_;
      ThreadLocal<Widget> wl;
      ++wl.get()->val_;
    });
  }).join();
  EXPECT_EQ(2, Widget::totalVal_);
}

// Test deleting the ThreadLocalPtr object
TEST(ThreadLocalPtr, CustomDeleter2) {
  Widget::totalVal_ = 0;
  std::thread t;
  std::mutex mutex;
  std::condition_variable cv;
  enum class State {
    START,
    DONE,
    EXIT,
  };
  State state = State::START;
  {
    ThreadLocalPtr<Widget> w;
    t = std::thread([&]() {
      w.reset(new Widget(), Widget::customDeleter);
      w.get()->val_ += 10;

      // Notify main thread that we're done
      {
        std::unique_lock lock(mutex);
        state = State::DONE;
        cv.notify_all();
      }

      // Wait for main thread to allow us to exit
      {
        std::unique_lock lock(mutex);
        while (state != State::EXIT) {
          cv.wait(lock);
        }
      }
    });

    // Wait for main thread to start (and set w.get()->val_)
    {
      std::unique_lock lock(mutex);
      while (state != State::DONE) {
        cv.wait(lock);
      }
    }

    // Thread started but hasn't exited yet
    EXPECT_EQ(0, Widget::totalVal_);

    // Destroy ThreadLocalPtr<Widget> (by letting it go out of scope)
  }

  EXPECT_EQ(1010, Widget::totalVal_);

  // Allow thread to exit
  {
    std::unique_lock lock(mutex);
    state = State::EXIT;
    cv.notify_all();
  }
  t.join();

  EXPECT_EQ(1010, Widget::totalVal_);
}

TEST(ThreadLocalPtr, SharedPtr) {
  ThreadLocalPtr<int> tlp;
  auto sp = std::make_shared<int>(7);
  EXPECT_EQ(1, sp.use_count());
  tlp.reset(sp);
  EXPECT_EQ(2, sp.use_count());
  EXPECT_EQ(sp.get(), tlp.get());
  tlp.reset();
  EXPECT_EQ(1, sp.use_count());
  EXPECT_EQ(static_cast<void*>(nullptr), tlp.get());
}

TEST(ThreadLocal, NotDefaultConstructible) {
  struct Object {
    int value;
    explicit Object(int v) : value{v} {}
  };
  std::atomic<int> a{};
  ThreadLocal<Object> o{[&a] { return Object(a++); }};
  EXPECT_EQ(0, o->value);
  std::thread([&] { EXPECT_EQ(1, o->value); }).join();
}

TEST(ThreadLocal, GetWithoutCreateUncreated) {
  Widget::totalVal_ = 0;
  Widget::totalMade_ = 0;
  ThreadLocal<Widget> w;
  std::thread([&w]() {
    auto ptr = w.get_existing();
    if (ptr) {
      ptr->val_++;
    }
  }).join();
  EXPECT_EQ(0, Widget::totalMade_);
}

TEST(ThreadLocal, GetWithoutCreateGets) {
  Widget::totalVal_ = 0;
  Widget::totalMade_ = 0;
  ThreadLocal<Widget> w;
  std::thread([&w]() {
    w->val_++;
    auto ptr = w.get_existing();
    if (ptr) {
      ptr->val_++;
    }
  }).join();
  EXPECT_EQ(1, Widget::totalMade_);
  EXPECT_EQ(2, Widget::totalVal_);
}

TEST(ThreadLocal, BasicDestructor) {
  Widget::totalVal_ = 0;
  ThreadLocal<Widget> w;
  std::thread([&w]() { w->val_ += 10; }).join();
  EXPECT_EQ(10, Widget::totalVal_);
}

TEST(ThreadLocal, MoveCtorFrom) {
  int calls = 0;
  ThreadLocal<int> src{[&] { return ++calls; }};
  EXPECT_EQ(1, *src);
  auto dst = static_cast<ThreadLocal<int>&&>(src);
  EXPECT_THROW(*src, std::bad_function_call);
  std::thread([&] { EXPECT_THROW(*src, std::bad_function_call); }).join();
  EXPECT_EQ(1, *dst);
  std::thread([&] { EXPECT_EQ(2, *dst); }).join();
}

TEST(ThreadLocal, MoveAssignFrom) {
  int calls = 0;
  ThreadLocal<int> src{[&] { return ++calls; }};
  EXPECT_EQ(1, *src);
  ThreadLocal<int> dst;
  dst = static_cast<ThreadLocal<int>&&>(src);
  EXPECT_THROW(*src, std::bad_function_call);
  std::thread([&] { EXPECT_THROW(*src, std::bad_function_call); }).join();
  EXPECT_EQ(1, *dst);
  std::thread([&] { EXPECT_EQ(2, *dst); }).join();
}

// this should force a realloc of the ElementWrapper array
TEST(ThreadLocal, ReallocDestructor) {
  ThreadLocal<MultiWidget> w;
  std::thread([&w]() { w->val_ += 10; }).join();
}

TEST(ThreadLocal, SimpleRepeatDestructor) {
  Widget::totalVal_ = 0;
  {
    ThreadLocal<Widget> w;
    w->val_ += 10;
  }
  {
    ThreadLocal<Widget> w;
    w->val_ += 10;
  }
  EXPECT_EQ(20, Widget::totalVal_);
}

TEST(ThreadLocal, InterleavedDestructors) {
  Widget::totalVal_ = 0;
  std::unique_ptr<ThreadLocal<Widget>> w;
  int wVersion = 0;
  const int wVersionMax = 2;
  int thIter = 0;
  std::mutex lock;
  auto th = std::thread([&]() {
    int wVersionPrev = 0;
    while (true) {
      while (true) {
        std::lock_guard g(lock);
        if (wVersion > wVersionMax) {
          return;
        }
        if (wVersion > wVersionPrev) {
          // We have a new version of w, so it should be initialized to zero
          EXPECT_EQ((*w)->val_, 0);
          break;
        }
      }
      std::lock_guard g(lock);
      wVersionPrev = wVersion;
      (*w)->val_ += 10;
      ++thIter;
    }
  });
  FOR_EACH_RANGE (i, 0, wVersionMax) {
    int thIterPrev = 0;
    {
      std::lock_guard g(lock);
      thIterPrev = thIter;
      w = std::make_unique<ThreadLocal<Widget>>();
      ++wVersion;
    }
    while (true) {
      std::lock_guard g(lock);
      if (thIter > thIterPrev) {
        break;
      }
    }
  }
  {
    std::lock_guard g(lock);
    wVersion = wVersionMax + 1;
  }
  th.join();
  EXPECT_EQ(wVersionMax * 10, Widget::totalVal_);
}

class SimpleThreadCachedInt {
  class NewTag;
  ThreadLocal<int, NewTag> val_;

 public:
  void add(int val) { *val_ += val; }

  int read() {
    int ret = 0;
    for (const auto& i : val_.accessAllThreads()) {
      ret += i;
    }
    return ret;
  }
};

TEST(ThreadLocalPtr, AccessAllThreadsCounter) {
  const int kNumThreads = 256;
  SimpleThreadCachedInt stci[kNumThreads + 1];
  std::atomic<bool> run(true);
  std::atomic<int> totalAtomic{0};
  std::vector<std::thread> threads;
  // thread i will increment all the thread locals
  // in the range 0..i
  for (int i = 0; i < kNumThreads; ++i) {
    threads.push_back(std::thread(
        [i, // i needs to be captured by value
         &stci,
         &run,
         &totalAtomic]() {
          for (int j = 0; j <= i; j++) {
            stci[j].add(1);
          }

          totalAtomic.fetch_add(1);
          while (run.load()) {
            usleep(100);
          }
        }));
  }
  while (totalAtomic.load() != kNumThreads) {
    usleep(100);
  }
  for (int i = 0; i <= kNumThreads; i++) {
    EXPECT_EQ(kNumThreads - i, stci[i].read());
  }
  run.store(false);
  for (auto& t : threads) {
    t.join();
  }
}

TEST(ThreadLocal, resetNull) {
  ThreadLocal<int> tl;
  tl.reset(new int(4));
  EXPECT_EQ(*tl.get(), 4);
  tl.reset();
  EXPECT_EQ(*tl.get(), 0);
  tl.reset(new int(5));
  EXPECT_EQ(*tl.get(), 5);
}

namespace {
struct Tag {};

struct Foo {
  folly::ThreadLocal<int, Tag> tl;
};
} // namespace

TEST(ThreadLocal, Movable1) {
  Foo a;
  Foo b;
  EXPECT_TRUE(a.tl.get() != b.tl.get());

  a = Foo();
  b = Foo();
  EXPECT_TRUE(a.tl.get() != b.tl.get());
}

TEST(ThreadLocal, Movable2) {
  std::map<int, Foo> map;

  map[42];
  map[10];
  map[23];
  map[100];

  std::set<void*> tls;
  for (auto& m : map) {
    tls.insert(m.second.tl.get());
  }

  // Make sure that we have 4 different instances of *tl
  EXPECT_EQ(4, tls.size());
}

namespace {

constexpr size_t kFillObjectSize = 300;

std::atomic<uint64_t> gDestroyed;

/**
 * Fill a chunk of memory with a unique-ish pattern that includes the thread id
 * (so deleting one of these from another thread would cause a failure)
 *
 * Verify it explicitly and on destruction.
 */
class FillObject {
 public:
  explicit FillObject(uint64_t idx) : idx_(idx) {
    uint64_t v = val();
    for (size_t i = 0; i < kFillObjectSize; ++i) {
      data_[i] = v;
    }
  }

  void check() {
    uint64_t v = val();
    for (size_t i = 0; i < kFillObjectSize; ++i) {
      CHECK_EQ(v, data_[i]);
    }
  }

  ~FillObject() { ++gDestroyed; }

 private:
  uint64_t val() const { return (idx_ << 40) | folly::getCurrentThreadID(); }

  uint64_t idx_;
  uint64_t data_[kFillObjectSize];
};

} // namespace

TEST(ThreadLocal, Stress) {
  static constexpr size_t numFillObjects = 250;
  std::array<ThreadLocalPtr<FillObject>, numFillObjects> objects;

  static constexpr size_t numThreads = 32;
  static constexpr size_t numReps = 20;

  std::vector<std::thread> threads;
  threads.reserve(numThreads);

  for (size_t k = 0; k < numThreads; ++k) {
    threads.emplace_back([&objects] {
      for (size_t rep = 0; rep < numReps; ++rep) {
        for (size_t i = 0; i < objects.size(); ++i) {
          objects[i].reset(new FillObject(rep * objects.size() + i));
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        for (size_t i = 0; i < objects.size(); ++i) {
          objects[i]->check();
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(numFillObjects * numThreads * numReps, gDestroyed);
}

struct StressAccessTag {};
using TLPInt = ThreadLocalPtr<int, Tag>;

static void tlpIntCustomDeleter(int* p, TLPDestructionMode /*unused*/) {
  delete p;
}

template <typename Op, typename Check>
void StressAccessTest(Op op, Check check) {
  static constexpr size_t kNumThreads = 16;
  static constexpr size_t kNumLoops = 10000;

  TLPInt ptr;
  ptr.reset(new int(0));
  std::atomic<bool> running{true};

  boost::barrier barrier(kNumThreads + 1);

  std::vector<std::thread> threads;

  for (size_t k = 0; k < kNumThreads; ++k) {
    threads.emplace_back([&] {
      ptr.reset(new int(1));

      barrier.wait();

      while (running.load()) {
        op(ptr);
      }
    });
  }

  // wait for the threads to be up and running
  barrier.wait();

  for (size_t n = 0; n < kNumLoops; n++) {
    int sum = 0;
    auto accessor = ptr.accessAllThreads();
    for (auto& i : accessor) {
      sum += i;
    }

    check(sum, kNumThreads);
  }

  running.store(false);
  for (auto& t : threads) {
    t.join();
  }
}

TEST(ThreadLocal, StressAccessReset) {
  StressAccessTest(
      [](TLPInt& ptr) { ptr.reset(new int(1)); },
      [](size_t sum, size_t numThreads) { EXPECT_EQ(sum, numThreads); });
}

TEST(ThreadLocal, StressAccessResetDeleter) {
  StressAccessTest(
      [](TLPInt& ptr) { ptr.reset(new int(1), tlpIntCustomDeleter); },
      [](size_t sum, size_t numThreads) { EXPECT_EQ(sum, numThreads); });
}

TEST(ThreadLocal, StressAccessRelease) {
  StressAccessTest(
      [](TLPInt& ptr) {
        auto* p = ptr.release();
        delete p;
        ptr.reset(new int(1));
      },
      [](size_t sum, size_t numThreads) { EXPECT_LE(sum, numThreads); });
}

// Yes, threads and fork don't mix
// (http://cppwisdom.quora.com/Why-threads-and-fork-dont-mix) but if you're
// stupid or desperate enough to try, we shouldn't stand in your way.
namespace {
class HoldsOne {
 public:
  HoldsOne() : value_(1) {}
  // Do an actual access to catch the buggy case where this == nullptr
  int value() const { return value_; }

 private:
  int value_;
};

struct HoldsOneTag {};

ThreadLocal<HoldsOne, HoldsOneTag> ptr;

int totalValue() {
  int value = 0;
  for (auto& p : ptr.accessAllThreads()) {
    value += p.value();
  }
  return value;
}

} // namespace

#ifdef FOLLY_HAVE_PTHREAD_ATFORK
TEST(ThreadLocal, Fork) {
  EXPECT_EQ(1, ptr->value()); // ensure created
  EXPECT_EQ(1, totalValue());
  // Spawn a new thread

  std::mutex mutex;
  bool started = false;
  std::condition_variable startedCond;
  std::atomic<bool> stopped = false;

  std::thread t([&]() {
    EXPECT_EQ(1, ptr->value()); // ensure created
    {
      std::unique_lock lock(mutex);
      started = true;
      startedCond.notify_all();
    }
    {
      while (!stopped) {
        // Keep invoking accessAllThreads which will acquire
        // the StaticMeta internal locks. The child() after fork should
        // not deadlock on the locks being inconsistent.
        EXPECT_EQ(2, totalValue());
        usleep(100); /* sleep override */
      }
    }
  });

  {
    std::unique_lock lock(mutex);
    while (!started) {
      startedCond.wait(lock);
    }
  }

  EXPECT_EQ(2, totalValue());

  pid_t pid = fork();
  if (pid == 0) {
    // in child
    int v = totalValue();

    // exit successfully if v == 1 (one thread)
    // diagnostic error code otherwise :)
    switch (v) {
      case 1:
        _exit(0);
      case 0:
        _exit(1);
    }
    _exit(2);
  } else if (pid > 0) {
    // in parent
    int status;
    EXPECT_EQ(pid, waitpid(pid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
  } else {
    ADD_FAILURE() << "fork failed";
  }

  EXPECT_EQ(2, totalValue());

  stopped = true;
  t.join();

  EXPECT_EQ(1, totalValue());
}
#endif

#ifndef _WIN32
struct HoldsOneTag2 {};

TEST(ThreadLocal, Fork2) {
  // A thread-local tag that was used in the parent from a *different* thread
  // (but not the forking thread) would cause the child to hang in a
  // ThreadLocalPtr's object destructor. Yeah.
  ThreadLocal<HoldsOne, HoldsOneTag2> p;
  {
    // use tag in different thread
    std::thread t([&p] { p.get(); });
    t.join();
  }
  pid_t pid = fork();
  if (pid == 0) {
    {
      ThreadLocal<HoldsOne, HoldsOneTag2> q;
      q.get();
    }
    _exit(0);
  } else if (pid > 0) {
    int status;
    EXPECT_EQ(pid, waitpid(pid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
  } else {
    ADD_FAILURE() << "fork failed";
  }
}

// Disable the SharedLibrary test when using any sanitizer. Otherwise, the
// dlopen'ed code would end up running without e.g., ASAN-initialized data
// structures and failing right away.
//
// We also cannot run this test unless folly was compiled with PIC support,
// since we cannot build thread_local_test_lib.so without PIC.
#if defined FOLLY_SANITIZE_ADDRESS || defined FOLLY_SANITIZE_THREAD || \
    !defined FOLLY_SUPPORT_SHARED_LIBRARY
#define SHARED_LIBRARY_TEST_NAME DISABLED_SharedLibrary
#else
#define SHARED_LIBRARY_TEST_NAME SharedLibrary
#endif

TEST(ThreadLocal, SHARED_LIBRARY_TEST_NAME) {
  auto const lib =
      folly::test::find_resource("folly/test/thread_local_test_lib.so");
  auto handle = dlopen(lib.string().c_str(), RTLD_LAZY);
  ASSERT_NE(nullptr, handle)
      << "unable to load " << lib.string() << ": " << dlerror();

  typedef void (*useA_t)();
  dlerror();
  useA_t useA = (useA_t)dlsym(handle, "useA");

  const char* dlsym_error = dlerror();
  EXPECT_EQ(nullptr, dlsym_error);
  ASSERT_NE(nullptr, useA);

  useA();

  folly::Baton<> b11, b12, b21, b22;

  std::thread t1([&]() {
    useA();
    b11.post();
    b12.wait();
  });

  std::thread t2([&]() {
    useA();
    b21.post();
    b22.wait();
  });

  b11.wait();
  b21.wait();

  dlclose(handle);

  b12.post();
  b22.post();

  t1.join();
  t2.join();
}

#endif
