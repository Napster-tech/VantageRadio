#pragma once

#include <cstdint>
#include <vector>
#include <string>

class ByteArray
{
public:
    // using Byte = std::byte;
    using Byte = uint8_t;

    ByteArray() {}
    ByteArray(const std::vector<Byte>& vec) : m_vec(vec) {}
    ByteArray(const Byte* data, size_t len) { m_vec.assign(data, data + len); }
    ByteArray(size_t count, Byte val) : m_vec(count, val) {}

    void clear() noexcept { m_vec.clear(); }

    bool isEmpty() const noexcept { return m_vec.empty(); }

    size_t size() const noexcept { return m_vec.size(); }
    void resize(size_t size) { m_vec.resize(size); }
    void truncate(size_t size) { m_vec.resize(size); }

    void reserve(size_t size) { m_vec.reserve(size); }
    size_t capacity() const noexcept { return m_vec.capacity(); }

    void append(const Byte& val) { m_vec.push_back(val); }
    void append(const ByteArray &data) { m_vec.insert(std::end(m_vec), std::begin(data.m_vec), std::end(data.m_vec)); }

    ByteArray left(int len) const;
    ByteArray mid(size_t index, int len = -1) const;

    Byte at(size_t n) const { return m_vec.at(n);  }

    Byte& operator[](const size_t pos) noexcept { return m_vec[pos]; }
    const Byte& operator[](const size_t pos) const noexcept { return m_vec[pos]; }

    Byte* data() noexcept { return m_vec.data(); }
    const Byte* data() const noexcept { return m_vec.data(); }
    const Byte* constData() const noexcept { return m_vec.data(); }

    std::string toHexString(char sep = 0) const;
    static ByteArray fromHexString(const std::string& str);

    bool operator==(const ByteArray& other) const { return (m_vec == other.m_vec); }
    bool operator!=(const ByteArray& other) const { return (m_vec != other.m_vec); }

private:
    std::vector<Byte> m_vec;
};
