#pragma once
#include <atomic>
#include <memory>
#include <optional>

// Callbacks and owner teardown run on the same UI apartment.
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

// Tracks one UI-apartment creation request. A canceled completion must not
// clear the pending state of a newer request.
class AsyncCreation {
public:
    std::optional<AsyncLifetime::Ticket> TryBegin() {
        if (pending) return std::nullopt;
        lifetime.Begin();
        pending = true;
        return lifetime.GetTicket();
    }
    bool Complete(const AsyncLifetime::Ticket& ticket) {
        if (!pending || !AsyncLifetime::IsCurrent(ticket)) return false;
        pending = false;
        lifetime.Cancel();
        return true;
    }
    void Cancel() { lifetime.Cancel(); pending = false; }
private:
    AsyncLifetime lifetime;
    bool pending = false;
};
