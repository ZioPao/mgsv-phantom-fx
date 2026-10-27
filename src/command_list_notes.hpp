#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace phantom_fx
{
// What a module notes while the game records. merge(later) adds the notes recorded after these, always in
// the order the immediate context runs them; each field says what later means (flags add up, the latest
// wins, lists append, the first stays).
template<class Notes>
concept ListNotes = std::default_initializable<Notes> && requires(Notes notes, Notes later) { notes.merge(std::move(later)); };

// A module's notes by deferred context while the game records, then by command list until they are handed
// over (docs/DEVELOPMENT.md, "Command lists"). Every call takes one lock, from any thread.
template<ListNotes Notes> class CommandListNotes
{
public:
    // change(Notes&) changes what `context` noted so far, under the lock.
    template<class Change> void note(std::uint64_t context, Change&& change)
    {
        std::lock_guard lock(mutex);
        change(byContext[context].notes);
    }

    // What `from` recorded now runs inside `into`, after what `into` holds. Never throws, as ReShade's events
    // call it: notes a failed allocation loses are gone.
    void recorded(std::uint64_t into, std::uint64_t from) noexcept
    {
        try
        {
            std::lock_guard lock(mutex);
            // FinishCommandList: a deferred context's notes become its new list's.
            if (const auto context = byContext.find(from); context != byContext.end()) moveNotes(byContext, context, byList, into);
            // A list run inside a deferred context joins the context's notes.
            else if (const auto list = byList.find(from); list != byList.end()) moveNotes(byList, list, byContext, into);
        }
        catch (...)
        {
        }
    }

    // The notes `list` recorded, as the immediate context is about to run it: handed over once.
    std::optional<Notes> take(std::uint64_t list)
    {
        std::lock_guard lock(mutex);
        const auto found = byList.find(list);
        if (found == byList.end()) return std::nullopt;

        auto notes = std::move(found->second.notes);
        byList.erase(found);

        return notes;
    }

    // use(waiting) under the lock: a range of the notes of every finished list not handed over yet, so
    // `use` can choose among them all.
    template<class Use> void withWaiting(Use&& use)
    {
        std::lock_guard lock(mutex);
        use(byList | std::views::transform([](auto& list) -> Notes& { return list.second.notes; }));
    }

    // A deferred context or command list is gone, and its address may come back.
    void forget(std::uint64_t contextOrList)
    {
        std::lock_guard lock(mutex);
        byContext.erase(contextOrList);
        byList.erase(contextOrList);
    }

    // At present: notes that waited through three presents expire at the fourth (a list never run, a
    // context never finished). Returns how many contexts' and lists' notes did.
    std::size_t present()
    {
        std::lock_guard lock(mutex);
        const auto expires = [](auto& entry) { return ++entry.second.presents > 3; };
        return std::erase_if(byContext, expires) + std::erase_if(byList, expires);
    }

    void clear()
    {
        std::lock_guard lock(mutex);
        byContext.clear();
        byList.clear();
    }

private:
    struct Entry
    {
        Notes notes;
        std::uint32_t presents{}; // Since its first note
    };
    using Entries = std::unordered_map<std::uint64_t, Entry>;

    // Moves `found`'s notes out of `source` to `into` in `target`, after any `into` has there, which keep
    // their age.
    static void moveNotes(Entries& source, typename Entries::iterator found, Entries& target, std::uint64_t into)
    {
        auto entry = std::move(found->second);
        source.erase(found);

        const auto existing = target.find(into);
        if (existing == target.end()) target.emplace(into, std::move(entry));
        else existing->second.notes.merge(std::move(entry.notes));
    }

    std::mutex mutex;
    Entries byContext; // While recorded
    Entries byList;    // Once finished, until handed over
};
}
