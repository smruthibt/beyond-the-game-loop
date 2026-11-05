#pragma once

#include "engine/GameObject.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// Owns every GameObject in a scene. Every network endpoint - server, each
// client, each peer - holds one, so the same object model runs everywhere.
//
// Ids are unique within a World and never reused while it lives. A World can
// start counting from any id, which is how endpoints keep their ids apart: a
// server counts from 1, a client counts its local-only objects from 1<<30, so
// an object the server sends with id 7 lands in the client's World as id 7
// and can never collide with anything the client made itself.
//
// Destroy() is deferred. The object is marked, skipped by every query from
// then on, and actually deleted at the next Flush(). Systems can therefore
// destroy objects while iterating, and a pointer taken earlier in a frame
// stays valid until the frame's Flush().
//
// Not internally synchronised. Like the rest of the world state, guard it
// with Engine::GetWorldMutex() when more than one thread touches it.
class World {
public:
    explicit World(ObjectId firstId = 1);

    GameObject &Create(const std::string &kind = {}, const std::string &name = {});

    // Creates an object with an id chosen elsewhere - the id the authority
    // gave it. If an object with that id already exists it is returned
    // as is (a destroyed one is revived, emptied of components).
    GameObject &CreateWithId(ObjectId id, const std::string &kind = {});

    // Nullptr if there is no such object or it has been destroyed.
    GameObject *Find(ObjectId id);
    const GameObject *Find(ObjectId id) const;
    GameObject *FindByName(const std::string &name);

    void Destroy(ObjectId id);
    // Destroys every object the predicate accepts. Returns how many.
    size_t DestroyIf(const std::function<bool(const GameObject &)> &predicate);

    // Deletes everything destroyed since the last Flush. Returns how many.
    size_t Flush();

    // Live objects only.
    size_t Count() const;
    std::vector<ObjectId> Ids() const;

    // Calls fn(object, components...) for every live object that has all of
    // Ts, in id order. Creating objects inside fn is allowed; whether the new
    // ones are visited in the same pass is unspecified.
    template <class... Ts, class F>
    void Each(F &&fn) {
        for (auto &entry : objects) {
            GameObject &object = *entry.second;
            if (object.destroyed) continue;
            if ((object.template Has<Ts>() && ...)) fn(object, *object.template Get<Ts>()...);
        }
    }
    template <class... Ts, class F>
    void Each(F &&fn) const {
        for (const auto &entry : objects) {
            const GameObject &object = *entry.second;
            if (object.destroyed) continue;
            if ((object.template Has<Ts>() && ...)) fn(object, *object.template Get<Ts>()...);
        }
    }

    // Removes everything at once, without waiting for a Flush.
    void Clear();

    ObjectId NextId() const { return nextId; }
    // Moves where Create() counts from - a peer, once it knows its id, moves
    // into its own range so its objects never collide with another peer's.
    void SetNextId(ObjectId id) { nextId = id == NO_OBJECT ? 1 : id; }

private:
    std::map<ObjectId, std::unique_ptr<GameObject>> objects;   // ordered: queries and snapshots are in id order
    std::vector<ObjectId> pending;
    ObjectId nextId;
};

} // namespace engine
