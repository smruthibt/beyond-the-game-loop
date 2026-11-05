#include "engine/World.hpp"

namespace engine {

// --- GameObject --------------------------------------------------------------

GameObject::GameObject(ObjectId objectId, std::string objectKind, std::string objectName)
    : id(objectId), kind(std::move(objectKind)), name(std::move(objectName)) {}

void GameObject::Attach(ComponentKey key, std::unique_ptr<Component> component) {
    component->owner = this;
    for (Slot &slot : components) {
        if (slot.key == key) {
            slot.component = std::move(component);
            slot.replicated = false;   // this process put it here now
            return;
        }
    }
    components.push_back(Slot{key, std::move(component), false});
}

void GameObject::MarkReplicated(ComponentKey key, bool replicated) {
    for (Slot &slot : components) {
        if (slot.key == key) slot.replicated = replicated;
    }
}

bool GameObject::IsReplicated(ComponentKey key) const {
    for (const Slot &slot : components) {
        if (slot.key == key) return slot.replicated;
    }
    return false;
}

Component *GameObject::Find(ComponentKey key) const {
    for (const Slot &slot : components) {
        if (slot.key == key) return slot.component.get();
    }
    return nullptr;
}

bool GameObject::Detach(ComponentKey key) {
    for (auto it = components.begin(); it != components.end(); ++it) {
        if (it->key == key) {
            components.erase(it);
            return true;
        }
    }
    return false;
}

// --- World -------------------------------------------------------------------

World::World(ObjectId firstId) : nextId(firstId == NO_OBJECT ? 1 : firstId) {}

GameObject &World::Create(const std::string &kind, const std::string &name) {
    while (objects.count(nextId)) ++nextId;   // ids handed in by CreateWithId may sit in our range
    const ObjectId id = nextId++;
    auto object = std::make_unique<GameObject>(id, kind, name);
    GameObject &ref = *object;
    objects.emplace(id, std::move(object));
    return ref;
}

GameObject &World::CreateWithId(ObjectId id, const std::string &kind) {
    auto it = objects.find(id);
    if (it != objects.end()) {
        GameObject &existing = *it->second;
        if (existing.destroyed) {
            // Destroyed and then told it exists again before the Flush: start
            // it over rather than resurrect stale components.
            existing.destroyed = false;
            existing.components.clear();
            existing.kind = kind;
        }
        return existing;
    }
    auto object = std::make_unique<GameObject>(id, kind);
    GameObject &ref = *object;
    objects.emplace(id, std::move(object));
    return ref;
}

GameObject *World::Find(ObjectId id) {
    auto it = objects.find(id);
    return it == objects.end() || it->second->destroyed ? nullptr : it->second.get();
}

const GameObject *World::Find(ObjectId id) const {
    auto it = objects.find(id);
    return it == objects.end() || it->second->destroyed ? nullptr : it->second.get();
}

GameObject *World::FindByName(const std::string &name) {
    for (auto &entry : objects) {
        if (!entry.second->destroyed && entry.second->name == name) return entry.second.get();
    }
    return nullptr;
}

void World::Destroy(ObjectId id) {
    auto it = objects.find(id);
    if (it == objects.end() || it->second->destroyed) return;
    it->second->destroyed = true;
    pending.push_back(id);
}

size_t World::DestroyIf(const std::function<bool(const GameObject &)> &predicate) {
    size_t n = 0;
    for (auto &entry : objects) {
        if (!entry.second->destroyed && predicate(*entry.second)) {
            entry.second->destroyed = true;
            pending.push_back(entry.first);
            ++n;
        }
    }
    return n;
}

size_t World::Flush() {
    size_t removed = 0;
    for (ObjectId id : pending) {
        auto it = objects.find(id);
        // Revived by CreateWithId since it was destroyed: keep it.
        if (it != objects.end() && it->second->destroyed) {
            objects.erase(it);
            ++removed;
        }
    }
    pending.clear();
    return removed;
}

size_t World::Count() const {
    size_t n = 0;
    for (const auto &entry : objects) {
        if (!entry.second->destroyed) ++n;
    }
    return n;
}

std::vector<ObjectId> World::Ids() const {
    std::vector<ObjectId> ids;
    ids.reserve(objects.size());
    for (const auto &entry : objects) {
        if (!entry.second->destroyed) ids.push_back(entry.first);
    }
    return ids;
}

void World::Clear() {
    objects.clear();
    pending.clear();
}

} // namespace engine
