//
// Created by kingdo on 2021/6/25.
//
#include <pistache/common.h>
#include <pistache/cookie.h>
#include <pistache/endpoint.h>
#include <pistache/http.h>
#include <pistache/http_headers.h>
#include <pistache/net.h>
#include <pistache/peer.h>
#include <sys/socket.h>
#include <sys/un.h>


#include <queue>

using namespace Pistache;
using namespace Pistache::Tcp;

Async::Promise<int> f()
{
    return Async::Promise<int>([=](Async::Deferred<int> deferred) {
        printf("1\n");
        //        deferred.resolve(2);
        //        deferred.reject(Pistache::Error::system("Could not write data"));
        //        deferred.reject(std::runtime_error("2"));
        //        deferred.reject();
        std::runtime_error("2");
        printf("4\n");
    });
}

[[maybe_unused]] int main2()
{

    f().then(
        [=](int a) {
            printf("-->%d<--\n", a);
        },
        [=](std::exception_ptr& eptr) {
            printf("31231\n");
            PrintException()(eptr);
        });
    printf("3\n");

    return 0;
}

void doSomeAsync(bool success, Async::Resolver& resolver, Async::Rejection& rejection)
{
    if (success)
        resolver(3);
    else
        rejection("3");
}

void doSomeAsync(const bool success, Async::Deferred<int>& deferred)
{
    if (success)
        deferred.resolve(3);
    else
        deferred.reject("3");
}

[[maybe_unused]] int main3()
{
    printf("1\n");
    bool success = true;
    Async::Promise<int> p([success](Async::Resolver& resolve,
                                    Async::Rejection& reject) {
        doSomeAsync(success, resolve, reject);
        printf("2\n");
    });
    Async::Promise<int> p1([success](Async::Deferred<int> deferred) {
        doSomeAsync(success, deferred);
        printf("2\n");
    });

    p.then(
        [](int a) {
            printf("%d\n", a);
        },
        [](std::exception_ptr& eptr) {
            try
            {
                std::rethrow_exception(eptr);
            }
            catch (const char* e)
            {
                printf("%s\n", e);
            }
        });
    printf("4");
    return 0;
}

int main4()
{
    Polling::Epoll poller;
    auto efd = eventfd(0, EFD_NONBLOCK);
    poller.addFd(efd, Flags<Polling::NotifyOn>(Polling::NotifyOn::Read), Polling::Tag(efd));
    std::vector<Polling::Event> events;
    eventfd_t val = 1;
    eventfd_write(efd, val);
    eventfd_write(efd, val);
    eventfd_write(efd, val);
    eventfd_read(efd, &val);
    printf("%lu\n", val);
    poller.poll(events);
    for (auto e : events)
    {
        if (e.tag == Polling::Tag(efd))
        {
            eventfd_read(efd, &val);
            printf("%lu\n", val);
        }
    }
    return 0;
}

int main5()
{
    Queue<double> queue;
    //    queue.push(1.1);
    queue.popSafe();
    sleep(1);
}
struct T
{
    short a;
    int n;
    long l;
    char c;
};
int main6()
{
    typedef typename std::aligned_storage<sizeof(int), alignof(int)>::type Storage;
    Storage storage;
    printf("alignof(T) = %zu\n",
           alignof(T));
}

class A
{
public:
    A(int a)
        : a(a)
    { }
    A(double b)
        : b(b)
    { }

private:
    int a;
    double b;
};

int main7()
{
    A a  = { 10 };
    A a1 = (10);
    A b  = { 10.1 };
}

int main(){

    struct sockaddr_in in{};
    struct sockaddr_un un;
    socklen_t t;
    size_t t2;

}