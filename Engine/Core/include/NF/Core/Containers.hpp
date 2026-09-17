#pragma once

// NF/Core/Containers.hpp — Engine containers
// Custom containers that use our allocators and avoid std overhead.

#include <NF/Core/Types.hpp>
#include <NF/Core/Assert.hpp>
#include <NF/Core/Memory.hpp>

#include <cstring>
#include <initializer_list>
#include <utility>

namespace nf {

// --- DynamicArray — growable array, allocator-aware ---
template<typename T>
class DynamicArray {
public:
    DynamicArray() = default;
    explicit DynamicArray(usize capacity) { reserve(capacity); }
    DynamicArray(usize count, const T& value) { resize(count, value); }
    DynamicArray(std::initializer_list<T> init) {
        reserve(init.size());
        for (const auto& v : init) {
            push_back(v);
        }
    }
    ~DynamicArray() { destroy_elements(); free_storage(); }

    DynamicArray(const DynamicArray& other) {
        reserve(other.m_size);
        for (usize i = 0; i < other.m_size; ++i) {
            new (m_data + i) T(other.m_data[i]);
        }
        m_size = other.m_size;
    }

    DynamicArray(DynamicArray&& other) noexcept {
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    DynamicArray& operator=(const DynamicArray& other) {
        if (this != &other) {
            destroy_elements();
            free_storage();
            reserve(other.m_size);
            for (usize i = 0; i < other.m_size; ++i) {
                new (m_data + i) T(other.m_data[i]);
            }
            m_size = other.m_size;
        }
        return *this;
    }

    DynamicArray& operator=(DynamicArray&& other) noexcept {
        if (this != &other) {
            destroy_elements();
            free_storage();
            m_data = other.m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    void push_back(const T& value) {
        if (m_size >= m_capacity) grow(m_capacity == 0 ? 4 : m_capacity * 2);
        new (m_data + m_size) T(value);
        ++m_size;
    }

    void push_back(T&& value) {
        if (m_size >= m_capacity) grow(m_capacity == 0 ? 4 : m_capacity * 2);
        new (m_data + m_size) T(std::move(value));
        ++m_size;
    }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        if (m_size >= m_capacity) grow(m_capacity == 0 ? 4 : m_capacity * 2);
        new (m_data + m_size) T(std::forward<Args>(args)...);
        ++m_size;
        return m_data[m_size - 1];
    }

    void pop_back() {
        NF_ASSERT(m_size > 0, "pop_back on empty array");
        --m_size;
        m_data[m_size].~T();
    }

    void reserve(usize new_capacity) {
        if (new_capacity > m_capacity) {
            grow(new_capacity);
        }
    }

    void resize(usize new_size, const T& value = T()) {
        if (new_size < m_size) {
            for (usize i = new_size; i < m_size; ++i) {
                m_data[i].~T();
            }
            m_size = new_size;
        } else if (new_size > m_size) {
            if (new_size > m_capacity) grow(new_size);
            for (usize i = m_size; i < new_size; ++i) {
                new (m_data + i) T(value);
            }
            m_size = new_size;
        }
    }

    void clear() {
        destroy_elements();
        m_size = 0;
    }

    void assign(usize count, const T& value) {
        clear();
        reserve(count);
        for (usize i = 0; i < count; ++i) {
            new (m_data + i) T(value);
        }
        m_size = count;
    }

    bool empty() const { return m_size == 0; }
    usize size() const { return m_size; }
    usize capacity() const { return m_capacity; }

    T*       data()       { return m_data; }
    const T* data() const { return m_data; }

    T&       operator[](usize i)       { NF_ASSERT(i < m_size, "index out of range"); return m_data[i]; }
    const T& operator[](usize i) const { NF_ASSERT(i < m_size, "index out of range"); return m_data[i]; }

    T*       begin()       { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const   { return m_data + m_size; }

    T&       front()       { NF_ASSERT(m_size > 0, "front on empty"); return m_data[0]; }
    T&       back()        { NF_ASSERT(m_size > 0, "back on empty"); return m_data[m_size - 1]; }

private:
    void grow(usize new_capacity) {
        T* new_data = reinterpret_cast<T*>(aligned_alloc(sizeof(T) * new_capacity, alignof(T)));
        NF_VERIFY(new_data != nullptr, "DynamicArray: allocation failed");

        for (usize i = 0; i < m_size; ++i) {
            new (new_data + i) T(std::move(m_data[i]));
            m_data[i].~T();
        }

        free_storage();
        m_data = new_data;
        m_capacity = new_capacity;
    }

    void destroy_elements() {
        for (usize i = 0; i < m_size; ++i) {
            m_data[i].~T();
        }
    }

    void free_storage() {
        if (m_data) {
            aligned_free(m_data);
            m_data = nullptr;
        }
        m_capacity = 0;
    }

    T*     m_data = nullptr;
    usize  m_size = 0;
    usize  m_capacity = 0;
};

// --- HashMap — open-addressing, linear probe ---
// Key must be hashable via std::hash
template<typename K, typename V>
class HashMap {
public:
    HashMap() = default;
    explicit HashMap(usize capacity) { reserve(capacity); }

    void put(const K& key, const V& value) {
        if (m_count * 4 >= m_capacity * 3) {
            rehash(m_capacity == 0 ? 8 : m_capacity * 2);
        }
        put_internal(key, value);
    }

    V* get(const K& key) {
        if (m_capacity == 0) return nullptr;
        usize idx = hash_index(key);
        for (usize i = 0; i < m_capacity; ++i) {
            usize slot = (idx + i) % m_capacity;
            if (!m_occupied[slot]) return nullptr;
            if (m_keys[slot] == key) return &m_values[slot];
        }
        return nullptr;
    }

    const V* get(const K& key) const {
        if (m_capacity == 0) return nullptr;
        usize idx = hash_index(key);
        for (usize i = 0; i < m_capacity; ++i) {
            usize slot = (idx + i) % m_capacity;
            if (!m_occupied[slot]) return nullptr;
            if (m_keys[slot] == key) return &m_values[slot];
        }
        return nullptr;
    }

    bool contains(const K& key) const { return get(key) != nullptr; }

    bool remove(const K& key) {
        if (m_capacity == 0) return false;
        usize idx = hash_index(key);
        for (usize i = 0; i < m_capacity; ++i) {
            usize slot = (idx + i) % m_capacity;
            if (!m_occupied[slot]) return false;
            if (m_keys[slot] == key) {
                m_occupied[slot] = false;
                --m_count;
                // Re-insert subsequent entries
                usize next = (slot + 1) % m_capacity;
                while (m_occupied[next]) {
                    K k = std::move(m_keys[next]);
                    V v = std::move(m_values[next]);
                    m_occupied[next] = false;
                    --m_count;
                    put_internal(k, v);
                    next = (next + 1) % m_capacity;
                }
                return true;
            }
        }
        return false;
    }

    void reserve(usize capacity) { rehash(capacity); }

    void clear() {
        m_keys.clear();
        m_values.clear();
        m_occupied.clear();
        m_capacity = 0;
        m_count = 0;
    }

    usize size() const { return m_count; }
    bool empty() const { return m_count == 0; }

private:
    usize hash_index(const K& key) const {
        return std::hash<K>{}(key) % m_capacity;
    }

    void put_internal(const K& key, const V& value) {
        usize idx = hash_index(key);
        for (usize i = 0; i < m_capacity; ++i) {
            usize slot = (idx + i) % m_capacity;
            if (!m_occupied[slot]) {
                m_keys[slot] = key;
                m_values[slot] = value;
                m_occupied[slot] = true;
                ++m_count;
                return;
            }
            if (m_keys[slot] == key) {
                m_values[slot] = value;
                return;
            }
        }
    }

    void rehash(usize new_capacity) {
        auto old_keys = std::move(m_keys);
        auto old_values = std::move(m_values);
        auto old_occupied = std::move(m_occupied);
        usize old_capacity = m_capacity;

        m_keys.resize(new_capacity);
        m_values.resize(new_capacity);
        m_occupied.assign(new_capacity, false);
        m_capacity = new_capacity;
        m_count = 0;

        for (usize i = 0; i < old_capacity; ++i) {
            if (old_occupied[i]) {
                put_internal(old_keys[i], old_values[i]);
            }
        }
    }

    DynamicArray<K>  m_keys;
    DynamicArray<V>  m_values;
    DynamicArray<bool> m_occupied;
    usize  m_capacity = 0;
    usize  m_count = 0;
};

} // namespace nf
