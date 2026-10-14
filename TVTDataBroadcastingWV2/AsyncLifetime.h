#pragma once
#include <atomic>
#include <memory>

// A ticket never owns the plugin. Invalidate it before teardown so delayed
// callbacks can reject a destroyed owner without dereferencing it.
class AsyncLifetime {
public:
    struct Token { std::atomic<bool> active{true}; };
    using Ticket = std::weak_ptr<Token>;
    void Begin() { Cancel(); token = std::make_shared<Token>(); }
    void Cancel() { if (token) token->active = false; token.reset(); }
    Ticket GetTicket() const { return token; }
    static bool IsCurrent(const Ticket& ticket) {
        const auto owner = ticket.lock();
        return owner && owner->active.load();
    }
    ~AsyncLifetime() { Cancel(); }
private:
    std::shared_ptr<Token> token;
};
