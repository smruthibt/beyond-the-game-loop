#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// Milestone 3, Part 1A: the runtime game object model.
//
// A GameObject is an id, a kind, a name and a bag of components. It has no
// behaviour of its own and no subclasses: what an object IS comes entirely
// from which components it carries. A player is Transform + RigidBody +
// Collider + Renderable + InputState + PlatformerController + NetIdentity; a
// death zone is Transform + Collider + DeathZone with no Renderable, which is
// what makes it hidden. A new kind of object is a new combination, and a new
// kind of behaviour is a new component type plus a system that reads it -
// neither needs a change to this file.
//
// Components hold data. Behaviour lives in systems (see Systems.hpp): plain
// functions that walk the World and act on every object holding the
// components they care about.

class Writer;
class Reader;
class GameObject;

using ObjectId = uint32_t;
constexpr ObjectId NO_OBJECT = 0;

// A component type's identity at runtime, without RTTI: the address of a
// static that exists once per type in the whole program.
using ComponentKey = const void *;
template <class T>
ComponentKey KeyOf() {
    static const char key = 0;
    return &key;
}

class Component {
public:
    virtual ~Component() = default;

    // The name this component travels under on the network, or nullptr for a
    // component that never leaves this process (a client's camera, say).
    // Use ENGINE_COMPONENT(Name) to declare it.
    virtual const char *TypeName() const { return nullptr; }

    // Every field the far end needs, in a fixed order. Read must consume
    // exactly what Write produced. Both default to nothing.
    virtual void Write(Writer &) const {}
    virtual void Read(Reader &) {}

    GameObject &Owner() const { return *owner; }

private:
    friend class GameObject;
    GameObject *owner = nullptr;
};

// Declares a component's network name. Inside the struct:
//     struct Health : engine::Component { ENGINE_COMPONENT(Health) int hp = 3; ... };
#define ENGINE_COMPONENT(Name)                                       \
    static constexpr const char *TYPE = #Name;                       \
    const char *TypeName() const override { return TYPE; }

class GameObject {
public:
    explicit GameObject(ObjectId id, std::string kind = {}, std::string name = {});
    GameObject(const GameObject &) = delete;
    GameObject &operator=(const GameObject &) = delete;

    ObjectId Id() const { return id; }

    // What sort of thing this is - "player", "platform" - as the game sees
    // it. It travels with the object, so a client that is told about a new
    // object can dress it (attach a sprite, a sound) by kind.
    const std::string &Kind() const { return kind; }
    void SetKind(std::string k) { kind = std::move(k); }

    // A human label for logs and lookups. Not replicated.
    const std::string &Name() const { return name; }
    void SetName(std::string n) { name = std::move(n); }

    // Adds a component, or replaces one of the same type.
    template <class T, class... Args>
    T &Add(Args &&...args) {
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T &ref = *component;
        Attach(KeyOf<T>(), std::move(component));
        return ref;
    }

    template <class T>
    T *Get() {
        return static_cast<T *>(Find(KeyOf<T>()));
    }
    template <class T>
    const T *Get() const {
        return static_cast<const T *>(Find(KeyOf<T>()));
    }
    template <class T>
    bool Has() const {
        return Find(KeyOf<T>()) != nullptr;
    }
    template <class T>
    bool Remove() {
        return Detach(KeyOf<T>());
    }

    // The untyped forms, for code that only knows a component by its key -
    // the network decoder building whatever the wire names.
    void Attach(ComponentKey key, std::unique_ptr<Component> component);
    Component *Find(ComponentKey key) const;
    bool Detach(ComponentKey key);

    struct Slot {
        ComponentKey key;
        std::unique_ptr<Component> component;
        // Put here by a snapshot rather than by this process's own code. Only
        // such components can be taken away again by a later snapshot; what a
        // game attached itself (a client dressing an object by kind) is never
        // removed by the network.
        bool replicated = false;
    };
    const std::vector<Slot> &Components() const { return components; }
    void MarkReplicated(ComponentKey key, bool replicated = true);
    bool IsReplicated(ComponentKey key) const;

    // True from World::Destroy() until the World's next Flush() removes it.
    bool IsDestroyed() const { return destroyed; }

private:
    friend class World;

    ObjectId id;
    std::string kind;
    std::string name;
    std::vector<Slot> components;   // a handful per object: a linear scan beats a map
    bool destroyed = false;
};

} // namespace engine
