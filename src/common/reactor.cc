/*
   Mathieu Stefani, 15 juin 2016

   Implementation of the Reactor
*/

#include <pistache/reactor.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::string_literals;

namespace Pistache::Aio
{

    /**
     * Impl的实现包括同步和异步两种实现，异步实现也是基于同步实现的
     * 基类Impl中，除实现了Reactor中的成员函数外，另外定义了run、runOnce、shutdown
     * */
    class Reactor::Impl
    {
    public:
        Impl(Reactor* reactor)
            : reactor_(reactor)
        { }

        virtual ~Impl() = default;

        virtual Reactor::Key addHandler(const std::shared_ptr<Handler>& handler,
                                        bool setKey)
            = 0;

        [[nodiscard]] virtual std::vector<std::shared_ptr<Handler>>
        handlers(const Reactor::Key& key) const = 0;

        virtual void registerFd(const Reactor::Key& key, Fd fd,
                                Polling::NotifyOn interest, Polling::Tag tag,
                                Polling::Mode mode = Polling::Mode::Level, bool exclusive = false)
            = 0;

        virtual void registerFdOneShot(const Reactor::Key& key, Fd fd,
                                       Polling::NotifyOn interest, Polling::Tag tag,
                                       Polling::Mode mode = Polling::Mode::Level)
            = 0;

        virtual void modifyFd(const Reactor::Key& key, Fd fd,
                              Polling::NotifyOn interest, Polling::Tag tag,
                              Polling::Mode mode = Polling::Mode::Level)
            = 0;

        virtual void removeFd(const Reactor::Key& key, Fd fd) = 0;

        virtual void runOnce() = 0;
        virtual void run()     = 0;

        virtual void shutdown() = 0;

        Reactor* reactor_;
    };

    /* Synchronous implementation of the reactor that polls in the context
     * of the same thread
     */
    /**
     * 同步实现，在一个线程中poll
     * 定义了一个handlerList handlers，handlerList中定义了一个数组存放所有的handler
     * 每个handler对应于一个线程
     * 这里需要注意，handler其实也是处理实际用户请求的函数方法
     * 这里需要这么理解，handler的成员函数对应了请求的处理方法
     * 而handler对象的私有变量，也就是context，记录了一个线程
     * 每一个线程都对应于一个handler
     *
     * 对于同步的实现而言，只有一个线程来处理请求，因此只会生成一个handler，也就是主线程
     * 而对于异步而言，会生成多个线程，对应于多个handler
     * 同步可以认为是只有一个线程的异步实现，因为i异步都是在调用同步的接口
     * */
    class SyncImpl : public Reactor::Impl
    {
    public:
        explicit SyncImpl(Reactor* reactor)
            : Reactor::Impl(reactor)
            , handlers_()
            , shutdown_()
            , shutdownFd()
            , poller()
        {
            shutdownFd.bind(poller);
        }
        // 将handler添加到SyncImpl的handlers
        // 同时执行handler的registerPoller函数，此函数在基类中是虚拟函数
        // 返回值是此handler在handlerList中的索引值
        Reactor::Key addHandler(const std::shared_ptr<Handler>& handler,
                                bool setKey = true) override
        {
            handler->registerPoller(poller);

            handler->reactor_ = reactor_;

            auto key = handlers_.add(handler);
            if (setKey)
                handler->key_ = key;

            return key;
        }
        // key就是handlers中每个handler的索引
        [[nodiscard]] std::shared_ptr<Handler> handler(const Reactor::Key& key) const
        {
            return handlers_.at(key.data());
        }
        // 将handler()的返回值打包到vector中
        [[nodiscard]] std::vector<std::shared_ptr<Handler>>
        handlers(const Reactor::Key& key) const override
        {
            std::vector<std::shared_ptr<Handler>> res;

            res.push_back(handler(key));
            return res;
        }

        /**
         * 下列所有操作fd的函数，同时包括了Reactor::Key和Polling::Tag
         *  key就是handler的index，tag则是文件描述符fd
         *  两者编码后重新生成新的Polling::Tag
         *  这样的话就能够知道那个fd由那个handler处理，其实一个fd就是一个用户的链接请求
         * */

        // 将fd注册到poller中，interest是感兴趣的事件
        void registerFd(const Reactor::Key& key, Fd fd, Polling::NotifyOn interest,
                        Polling::Tag tag,
                        Polling::Mode mode = Polling::Mode::Level, bool exclusive = false) override
        {
            // encodeTag将fd和index值编码成新的polling::tag
            // 编码的规则是索引值在uint64的高8位，fd在低位
            auto pollTag = encodeTag(key, tag);
            poller.addFd(fd, Flags<Polling::NotifyOn>(interest), pollTag, mode, exclusive);
        }

        // 使用OneShot Flags，即一旦触发事件，则关闭对fd的轮循，直到再次绑定，也就是执行modifyFd
        void registerFdOneShot(const Reactor::Key& key, Fd fd,
                               Polling::NotifyOn interest, Polling::Tag tag,
                               Polling::Mode mode = Polling::Mode::Level) override
        {

            auto pollTag = encodeTag(key, tag);
            poller.addFdOneShot(fd, Flags<Polling::NotifyOn>(interest), pollTag, mode);
        }

        // 此函数调用rearmFd，用于解除对EPOLLONESHOT的影响，其操作和registerFd几乎完全相同
        // 仅仅是modifyFd是用的是EPOLL_CTL_MOD,而registerFd使用的是EPOLL_CTL_ADD
        void modifyFd(const Reactor::Key& key, Fd fd, Polling::NotifyOn interest,
                      Polling::Tag tag,
                      Polling::Mode mode = Polling::Mode::Level) override
        {

            auto pollTag = encodeTag(key, tag);
            poller.rearmFd(fd, Flags<Polling::NotifyOn>(interest), pollTag, mode);
        }

        void removeFd(const Reactor::Key& /*key*/, Fd fd) override
        {
            poller.removeFd(fd);
        }

        // 真正的开始执行轮循
        // 每个线程都有自己的轮循器
        // 当接收到事件之后，将会触发handleFds来处理事件
        void runOnce() override
        {
            if (handlers_.empty())
                throw std::runtime_error("You need to set at least one handler");

            for (;;)
            {
                std::vector<Polling::Event> events;
                int ready_fds = poller.poll(events);

                switch (ready_fds)
                {
                case -1:
                    break;
                case 0:
                    break;
                default:
                    if (shutdown_)
                        return;

                    handleFds(std::move(events));
                }
            }
        }

        void run() override
        {
            // 设置handler的context_，也就是每个handler对应的线程的tid
            handlers_.forEachHandler([](const std::shared_ptr<Handler> handler) {
                handler->context_.tid = std::this_thread::get_id();
            });

            while (!shutdown_)
                runOnce();
        }

        void shutdown() override
        {
            shutdown_.store(true);
            shutdownFd.notify();
        }

        static constexpr size_t MaxHandlers() { return HandlerList::MaxHandlers; }

    private:
        static Polling::Tag encodeTag(const Reactor::Key& key, Polling::Tag tag)
        {
            uint64_t value = tag.value();
            return HandlerList::encodeTag(key, value);
        }

        static std::pair<size_t, uint64_t> decodeTag(const Polling::Tag& tag)
        {
            return HandlerList::decodeTag(tag);
        }

        void handleFds(std::vector<Polling::Event> events) const
        {
            // Fast-path: if we only have one handler, do not bother scanning the fds to
            // find the right handlers
            if (handlers_.size() == 1)
                handlers_.at(0)->onReady(FdSet(std::move(events)));
            else
            {
                std::unordered_map<std::shared_ptr<Handler>, std::vector<Polling::Event>>
                    fdHandlers;

                // 因为每个事件的tag是由index和fd组成
                // 在这里我们根据index将所有的event分类，让handler处理自己的
                for (auto& event : events)
                {
                    size_t index;
                    uint64_t value;

                    std::tie(index, value) = decodeTag(event.tag);
                    auto handler_          = handlers_.at(index);
                    auto& evs              = fdHandlers.at(handler_);
                    evs.push_back(std::move(event));
                }

                for (auto& data : fdHandlers)
                {
                    data.first->onReady(FdSet(std::move(data.second)));
                }
            }
        }

        struct HandlerList
        {

            // We are using the highest 8 bits of the fd to encode the index of the
            // handler, which gives us a maximum of 2**8 - 1 handler, 255
            static constexpr size_t HandlerBits  = 8;
            static constexpr size_t HandlerShift = sizeof(uint64_t) - HandlerBits;
            static constexpr uint64_t DataMask   = uint64_t(-1) >> HandlerBits;

            static constexpr size_t MaxHandlers = (1 << HandlerBits) - 1;

            HandlerList()
                : handlers()
                , index_()
            {
                std::fill(std::begin(handlers), std::end(handlers), nullptr);
            }

            HandlerList(const HandlerList& other)            = delete;
            HandlerList& operator=(const HandlerList& other) = delete;

            HandlerList(HandlerList&& other)            = default;
            HandlerList& operator=(HandlerList&& other) = default;

            // 基于handlers，clone一个新的HandlerList，
            // 需要调用handlers的clone函数
            [[nodiscard]] HandlerList clone() const
            {
                HandlerList list;

                for (size_t i = 0; i < index_; ++i)
                {
                    list.handlers.at(i) = handlers.at(i)->clone();
                }
                list.index_ = index_;

                return list;
            }

            Reactor::Key add(const std::shared_ptr<Handler>& handler)
            {
                if (index_ == MaxHandlers)
                    throw std::runtime_error("Maximum handlers reached");

                Reactor::Key key(index_);
                handlers.at(index_++) = handler;

                return key;
            }

            std::shared_ptr<Handler> operator[](size_t index) const
            {
                return handlers.at(index);
            }

            [[nodiscard]] std::shared_ptr<Handler> at(size_t index) const
            {
                if (index >= index_)
                    throw std::runtime_error("Attempting to retrieve invalid handler");

                return handlers.at(index);
            }

            [[nodiscard]] bool empty() const { return index_ == 0; }

            [[nodiscard]] size_t size() const { return index_; }

            // 这里的index就是handlers的索引值，value就是文件描述符值
            // 编码的规则是索引值在uint64的高8位，fd在低位
            static Polling::Tag encodeTag(const Reactor::Key& key, uint64_t value)
            {
                auto index = key.data();
                // The reason why we are using the most significant bits to encode
                // the index of the handler is that in the fast path, we won't need
                // to shift the value to retrieve the fd if there is only one handler as
                // all the bits will already be set to 0.
                auto encodedValue = (index << HandlerShift) | value;
                return Polling::Tag(encodedValue);
            }

            static std::pair<size_t, uint64_t> decodeTag(const Polling::Tag& tag)
            {
                auto value   = tag.value();
                size_t index = value >> HandlerShift;
                uint64_t fd  = value & DataMask;

                return std::make_pair(index, fd);
            }

            template <typename Func>
            void forEachHandler(Func func) const
            {
                for (size_t i = 0; i < index_; ++i)
                    func(handlers.at(i));
            }

        private:
            std::array<std::shared_ptr<Handler>, MaxHandlers> handlers;
            size_t index_;
        };

        HandlerList handlers_;

        std::atomic<bool> shutdown_;
        NotifyFd shutdownFd;

        Polling::Epoll poller;
    };

    /* Asynchronous implementation of the reactor that spawns a number N of threads
     * and creates a polling fd per thread
     *
     * Implementation detail:
     *
     *  Here is how it works: the implementation simply starts a synchronous variant
     *  of the implementation in its own std::thread. When adding an handler, it
     * will add a clone() of the handler to every worker (thread), and assign its
     * own key to the handler. Here is where things start to get interesting. Here
     * is how the key encoding works for every handler:
     *
     *  [     handler idx      ] [       worker idx         ]
     *  ------------------------ ----------------------------
     *       ^ 32 bits                   ^ 32 bits
     *  -----------------------------------------------------
     *                       ^ 64 bits
     *
     * Since we have up to 64 bits of data for every key, we encode the index of the
     * handler that has been assigned by the SyncImpl in the upper 32 bits, and
     * encode the index of the worker thread in the lowest 32 bits.
     *
     * When registering a fd for a given key, the AsyncImpl then knows which worker
     * to use by looking at the lowest 32 bits of the Key's data. The SyncImpl will
     * then use the highest 32 bits to retrieve the index of the handler.
     */
    /**
     * 异步实现Reactor，它产生 N 个线程并为每个线程创建一个epoll-fd,实现细节：
     *
     * */

    class AsyncImpl : public Reactor::Impl
    {
    public:
        static constexpr uint32_t KeyMarker = 0xBADB0B;

        AsyncImpl(Reactor* reactor, size_t threads, const std::string& threadsName)
            : Reactor::Impl(reactor)
        {

            if (threads > SyncImpl::MaxHandlers())
                throw std::runtime_error("Too many worker threads requested (max "s + std::to_string(SyncImpl::MaxHandlers()) + ")."s);

            for (size_t i = 0; i < threads; ++i)
                workers_.emplace_back(std::make_unique<Worker>(reactor, threadsName));
        }

        Reactor::Key addHandler(const std::shared_ptr<Handler>& handler,
                                bool) override
        {

            std::array<Reactor::Key, SyncImpl::MaxHandlers()> keys;

            for (size_t i = 0; i < workers_.size(); ++i)
            {
                auto& wrk = workers_.at(i);

                auto cl     = handler->clone();
                auto key    = wrk->sync->addHandler(cl, false /* setKey */);
                auto newKey = encodeKey(key, static_cast<uint32_t>(i));
                cl->key_    = newKey;

                keys.at(i) = key;
            }

            auto data = keys.at(0).data() << 32 | KeyMarker;

            return Reactor::Key(data);
        }

        std::vector<std::shared_ptr<Handler>>
        handlers(const Reactor::Key& key) const override
        {

            const std::pair<uint32_t, uint32_t> idx_marker = decodeKey(key);
            if (idx_marker.second != KeyMarker)
                throw std::runtime_error("Invalid key");

            Reactor::Key originalKey(idx_marker.first);

            std::vector<std::shared_ptr<Handler>> res;
            res.reserve(workers_.size());
            for (const auto& wrk : workers_)
            {
                res.push_back(wrk->sync->handler(originalKey));
            }

            return res;
        }

        void registerFd(const Reactor::Key& key, Fd fd, Polling::NotifyOn interest,
                        Polling::Tag tag,
                        Polling::Mode mode = Polling::Mode::Level, bool exclusive = false) override
        {
            dispatchCall(key, &SyncImpl::registerFd, fd, interest, tag, mode, exclusive);
        }

        void registerFdOneShot(const Reactor::Key& key, Fd fd,
                               Polling::NotifyOn interest, Polling::Tag tag,
                               Polling::Mode mode = Polling::Mode::Level) override
        {
            dispatchCall(key, &SyncImpl::registerFdOneShot, fd, interest, tag, mode);
        }

        void modifyFd(const Reactor::Key& key, Fd fd, Polling::NotifyOn interest,
                      Polling::Tag tag,
                      Polling::Mode mode = Polling::Mode::Level) override
        {
            dispatchCall(key, &SyncImpl::modifyFd, fd, interest, tag, mode);
        }

        void removeFd(const Reactor::Key& key, Fd fd) override
        {
            dispatchCall(key, &SyncImpl::removeFd, fd);
        }

        void runOnce() override { }

        void run() override
        {
            for (auto& wrk : workers_)
                wrk->run();
        }

        void shutdown() override
        {
            for (auto& wrk : workers_)
                wrk->shutdown();
        }

    private:
        static Reactor::Key encodeKey(const Reactor::Key& originalKey,
                                      uint32_t value)
        {
            auto data     = originalKey.data();
            auto newValue = data << 32 | value;
            return Reactor::Key(newValue);
        }

        static std::pair<uint32_t, uint32_t>
        decodeKey(const Reactor::Key& encodedKey)
        {
            auto data = encodedKey.data();
            auto hi   = static_cast<uint32_t>(data >> 32);
            auto lo   = static_cast<uint32_t>(data & 0xFFFFFFFF);
            return std::make_pair(hi, lo);
        }

#define CALL_MEMBER_FN(obj, pmf) (obj->*(pmf))

        template <typename Func, typename... Args>
        void dispatchCall(const Reactor::Key& key, Func func, Args&&... args) const
        {
            auto decoded    = decodeKey(key);
            const auto& wrk = workers_.at(decoded.second);

            Reactor::Key originalKey(decoded.first);
            CALL_MEMBER_FN(wrk->sync.get(), func)
            (originalKey, std::forward<Args>(args)...);
        }

#undef CALL_MEMBER_FN

        struct Worker
        {

            explicit Worker(Reactor* reactor, const std::string& threadsName)
                : thread()
                , sync(new SyncImpl(reactor))
                , threadsName_(threadsName)
            { }

            ~Worker()
            {
                if (thread.joinable())
                    thread.join();
            }

            void run()
            {
                thread = std::thread([=]() {
                    if (!threadsName_.empty())
                    {
                        pthread_setname_np(pthread_self(),
                                           threadsName_.substr(0, 15).c_str());
                    }
                    sync->run();
                });
            }

            void shutdown() { sync->shutdown(); }

            std::thread thread;
            std::unique_ptr<SyncImpl> sync;
            std::string threadsName_;
        };

        std::vector<std::unique_ptr<Worker>> workers_;
    };

    Reactor::Key::Key()
        : data_(0)
    { }

    Reactor::Key::Key(uint64_t data)
        : data_(data)
    { }

    Reactor::Reactor() = default;

    Reactor::~Reactor() = default;

    std::shared_ptr<Reactor> Reactor::create()
    {
        return std::make_shared<Reactor>();
    }

    void Reactor::init()
    {
        SyncContext context;
        init(context);
    }

    void Reactor::init(const ExecutionContext& context)
    {
        impl_.reset(context.makeImpl(this));
    }

    Reactor::Key Reactor::addHandler(const std::shared_ptr<Handler>& handler)
    {
        return impl()->addHandler(handler, true);
    }

    std::vector<std::shared_ptr<Handler>>
    Reactor::handlers(const Reactor::Key& key)
    {
        return impl()->handlers(key);
    }

    void Reactor::registerFd(const Reactor::Key& key, Fd fd,
                             Polling::NotifyOn interest, Polling::Tag tag,
                             Polling::Mode mode, bool exclusive)
    {
        impl()->registerFd(key, fd, interest, tag, mode, exclusive);
    }

    void Reactor::registerFdOneShot(const Reactor::Key& key, Fd fd,
                                    Polling::NotifyOn interest, Polling::Tag tag,
                                    Polling::Mode mode)
    {
        impl()->registerFdOneShot(key, fd, interest, tag, mode);
    }

    void Reactor::registerFd(const Reactor::Key& key, Fd fd,
                             Polling::NotifyOn interest, Polling::Mode mode, bool exclusive)
    {
        impl()->registerFd(key, fd, interest, Polling::Tag(fd), mode, exclusive);
    }

    void Reactor::registerFdOneShot(const Reactor::Key& key, Fd fd,
                                    Polling::NotifyOn interest,
                                    Polling::Mode mode)
    {
        impl()->registerFdOneShot(key, fd, interest, Polling::Tag(fd), mode);
    }

    void Reactor::modifyFd(const Reactor::Key& key, Fd fd,
                           Polling::NotifyOn interest, Polling::Tag tag,
                           Polling::Mode mode)
    {
        impl()->modifyFd(key, fd, interest, tag, mode);
    }

    void Reactor::modifyFd(const Reactor::Key& key, Fd fd,
                           Polling::NotifyOn interest, Polling::Mode mode)
    {
        impl()->modifyFd(key, fd, interest, Polling::Tag(fd), mode);
    }

    void Reactor::removeFd(const Reactor::Key& key, Fd fd)
    {
        impl()->removeFd(key, fd);
    }

    void Reactor::run() { impl()->run(); }

    void Reactor::shutdown()
    {
        if (impl_)
            impl()->shutdown();
    }

    void Reactor::runOnce() { impl()->runOnce(); }

    Reactor::Impl* Reactor::impl() const
    {
        if (!impl_)
            throw std::runtime_error(
                "Invalid object state, you should call init() before.");

        return impl_.get();
    }

    Reactor::Impl* SyncContext::makeImpl(Reactor* reactor) const
    {
        return new SyncImpl(reactor);
    }

    Reactor::Impl* AsyncContext::makeImpl(Reactor* reactor) const
    {
        return new AsyncImpl(reactor, threads_, threadsName_);
    }

    AsyncContext AsyncContext::singleThreaded() { return AsyncContext(1); }

} // namespace Pistache::Aio
