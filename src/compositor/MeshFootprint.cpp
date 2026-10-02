#include "compositor/MeshFootprint.h"

#include <cmath>

namespace tg::compositor {

namespace {

uint64_t HashMix(uint64_t hash, uint64_t value) {
    // FNV-1a を 64 bit の値ごとに回す。
    hash ^= value;
    hash *= 0x100000001b3ull;
    return hash;
}

int64_t Quantize(float value, float step) {
    return static_cast<int64_t>(std::llround(static_cast<double>(value) / step));
}

}  // namespace

uint64_t HashMeshFootprint(const MeshFootprint& footprint) {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = HashMix(hash, footprint.vertices.size());
    hash = HashMix(hash, footprint.indices.size());
    for (const MeshFootprintVertex& vertex : footprint.vertices) {
        hash = HashMix(hash, static_cast<uint64_t>(Quantize(vertex.u, 1.0f / 65536.0f)));
        hash = HashMix(hash, static_cast<uint64_t>(Quantize(vertex.v, 1.0f / 65536.0f)));
        hash = HashMix(hash, static_cast<uint64_t>(Quantize(vertex.height, 1.0f / 4096.0f)));
    }
    for (const uint32_t index : footprint.indices) {
        hash = HashMix(hash, index);
    }
    return hash;
}

bool MeshFootprintStore::Set(uint32_t key, MeshFootprint footprint) {
    footprint.hash = HashMeshFootprint(footprint);
    const auto found = m_entries.find(key);
    if (found != m_entries.end() && found->second.hash == footprint.hash) {
        return false;
    }
    m_entries[key] = std::move(footprint);
    ++m_revision;
    return true;
}

bool MeshFootprintStore::Remove(uint32_t key) {
    if (m_entries.erase(key) == 0) {
        return false;
    }
    ++m_revision;
    return true;
}

const MeshFootprint* MeshFootprintStore::Find(uint32_t key) const {
    const auto found = m_entries.find(key);
    return found == m_entries.end() ? nullptr : &found->second;
}

std::vector<uint32_t> MeshFootprintStore::Keys() const {
    std::vector<uint32_t> keys;
    keys.reserve(m_entries.size());
    for (const auto& [key, entry] : m_entries) {
        keys.push_back(key);
    }
    return keys;
}

}  // namespace tg::compositor
