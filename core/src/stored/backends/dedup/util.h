/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_
#define BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_

#include <fcntl.h>
#include <unistd.h>
#include <memory>
#include <utility>

namespace dedup::util {
static_assert(((ssize_t)(off_t)-1) < 0,
              "The code assumes that the error condition is negative when cast "
              "to ssize_t");
class raii_fd {
 public:
  raii_fd() = default;
  raii_fd(const char* path, int flags, int mode) : flags{flags}, mode{mode}
  {
    fd = ::open(path, flags, mode);
  }

  raii_fd(int dird, const char* path, int flags, int mode)
      : flags{flags}, mode{mode}
  {
    fd = openat(dird, path, flags, mode);
  }

  raii_fd(raii_fd&& move_from) : raii_fd{} { *this = std::move(move_from); }

  raii_fd& operator=(raii_fd&& move_from)
  {
    std::swap(fd, move_from.fd);
    std::swap(flags, move_from.flags);
    std::swap(mode, move_from.mode);
    return *this;
  }

  bool is_ok() const { return !(fd < 0 || error); }

  int get() const { return fd; }

  bool flush() { return ::fsync(fd) == 0; }

  bool resize(std::size_t new_size) { return ::ftruncate(fd, new_size) == 0; }

  bool write(const void* data, std::size_t size)
  {
    ssize_t res = ::write(fd, data, size);
    if (res < 0) {
      error = true;
      return false;
    }

    if (static_cast<std::size_t>(res) != size) {
      error = true;
      return false;
    }

    return true;
  }

  bool read(void* data, std::size_t size)
  {
    ssize_t res = ::read(fd, data, size);
    if (res < 0) {
      error = true;
      return false;
    }

    if (static_cast<std::size_t>(res) != size) {
      error = true;
      return false;
    }

    return true;
  }

  bool seek(std::size_t where)
  {
    ssize_t res = ::lseek(fd, where, SEEK_SET);
    if (res < 0) { return false; }
    return static_cast<std::size_t>(res) == where;
  }

  std::optional<std::size_t> size_then_reset()
  {
    ssize_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
      error = true;
      return std::nullopt;
    }
    if (::lseek(fd, 0, SEEK_SET) != 0) {
      error = true;
      return std::nullopt;
    }
    return static_cast<std::size_t>(size);
  }

  ~raii_fd()
  {
    if (fd >= 0) { close(fd); }
  }

 private:
  int fd{-1};
  int flags{};
  int mode{};
  bool error{false};
};

template <typename T> class file_based_vector {
 public:
  file_based_vector() = default;
  file_based_vector(raii_fd file,
                    std::size_t used,
                    std::size_t capacity_chunk_size);
  file_based_vector(file_based_vector&& other) : file_based_vector{}
  {
    *this = std::move(other);
  }
  file_based_vector& operator=(file_based_vector&& other);

  std::optional<std::size_t> reserve(std::size_t count);
  std::optional<std::size_t> write(const T* arr, std::size_t count);
  std::optional<std::size_t> write_at(std::size_t start,
                                      const T* arr,
                                      std::size_t count);
  inline std::optional<std::size_t> write(const T& val)
  {
    return write(&val, 1);
  }
  inline std::optional<std::size_t> write_at(std::size_t start, const T& val)
  {
    return write_at(start, &val, 1);
  }
  std::unique_ptr<T[]> read(std::size_t count = 1);
  std::unique_ptr<T[]> read_at(std::size_t start, std::size_t count = 1);
  std::unique_ptr<T[]> peek(std::size_t count = 1);

  bool move_to(std::size_t start);

  bool flush()
  {
    if (error) { return false; }
    // if we used a cache we would write it out here
    return file.flush();
  }

  inline std::size_t size() const { return used; }

  inline std::size_t current() const { return iter; }

  inline bool is_ok() const { return !error && file.is_ok(); }

 private:
  // future: std::vector<T> cache;
  std::size_t used{0};
  std::size_t capacity;
  std::size_t iter{0};
  std::size_t capacity_chunk_size{1};
  raii_fd file;
  bool error{true};
  static constexpr std::size_t elem_size = sizeof(T);

  std::optional<std::size_t> reserve_at(std::size_t at, std::size_t count);
};

template <typename T>
std::optional<std::size_t> file_based_vector<T>::reserve(std::size_t count)
{
  std::optional start = reserve_at(used, count);

  if (start.has_value()) { iter = used; }

  return start;
}

template <typename T>
std::optional<std::size_t> file_based_vector<T>::reserve_at(std::size_t at,
                                                            std::size_t count)
{
  if (error) { return std::nullopt; }

  if (at + count < at) {
    return std::nullopt;  // make sure nothing weird is going on
  }

  if (at > used) {
    // since this is an internal function
    // this should never happen.  So if it does set the error flag
    error = true;
    return std::nullopt;
  }

  if (at + count > capacity) {
    std::size_t delta = at + count - capacity;

    // compute first mutilple of capacity_chunk_size that is greater than delta
    std::size_t num_new_items
        = ((delta + capacity_chunk_size - 1) / capacity_chunk_size)
          * capacity_chunk_size;
    ASSERT(num_new_items + capacity >= at + count);

    std::size_t new_cap = capacity + num_new_items;
    if (new_cap < at + count) {
      // something weird is going on :S
      return std::nullopt;
    }

    if (!file.resize(new_cap * elem_size)) {
      error = true;
      return std::nullopt;
    }

    capacity = new_cap;
  }

  used = std::max(used, at + count);
  return at;
}

template <typename T>
std::optional<std::size_t> file_based_vector<T>::write(const T* arr,
                                                       std::size_t count)
{
  std::optional start = reserve_at(iter, count);

  if (!start) { return std::nullopt; }

  ASSERT(start == iter);
  auto old_iter = iter;
  iter += count;
  // read_at always returns to the current iter
  // if we set iter to the new desired position
  // we prevent the double seek we would need to do otherwise
  auto res = write_at(old_iter, arr, count);

  if (!res) { iter = old_iter; }

  return res;
}

template <typename T>
std::optional<std::size_t> file_based_vector<T>::write_at(std::size_t start,
                                                          const T* arr,
                                                          std::size_t count)
{
  if (error) { return std::nullopt; }

  if (start > used) { return std::nullopt; }

  if (!file.seek(elem_size * start)) {
    error = true;
    return std::nullopt;
  }

  if (!file.write(arr, count * elem_size)) {
    error = true;
    return std::nullopt;
  }

  // go back to normal place
  if (!file.seek(elem_size * iter)) {
    error = true;
    return std::nullopt;
  }

  return start;
}

template <typename T>
std::unique_ptr<T[]> file_based_vector<T>::read(std::size_t count)
{
  if (error) { return nullptr; }

  auto old_iter = iter;
  iter += count;
  // read_at always returns to the current iter
  // if we set iter to the new desired position
  // we prevent the double seek we would need to do otherwise
  std::unique_ptr result = read_at(old_iter, count);

  if (!result) { iter = old_iter; }

  return result;
}

template <typename T>
std::unique_ptr<T[]> file_based_vector<T>::read_at(std::size_t start,
                                                   std::size_t count)
{
  if (error) { return nullptr; }

  if (start + count > used) { return nullptr; }

  std::unique_ptr<T[]> data(new T[count]);
  T* arr = data.get();

  if (!file.seek(elem_size * start)) {
    error = true;
    return nullptr;
  }

  if (!file.read(arr, count * elem_size)) {
    error = true;
    return nullptr;
  }

  // go back to normal place
  if (!file.seek(elem_size * iter)) {
    error = true;
    return nullptr;
  }

  return data;
}

template <typename T>
std::unique_ptr<T[]> file_based_vector<T>::peek(std::size_t count)
{
  if (error) { return nullptr; }
  return read_at(iter, count);
}

template <typename T> bool file_based_vector<T>::move_to(std::size_t start)
{
  if (error) { return false; }

  if (start > used) { return false; }

  if (iter == start) { return true; }

  iter = start;

  if (!file.seek(iter * elem_size)) {
    error = true;
    return false;
  }
  return true;
}

template <typename T>
file_based_vector<T>::file_based_vector(raii_fd file_,
                                        std::size_t used_,
                                        std::size_t capacity_chunk_size_)
    : used{used_}
    , capacity{0}
    , capacity_chunk_size(capacity_chunk_size_)
    , file{std::move(file_)}
    , error{!file.is_ok()}
{
  if (error) { return; }

  // let us compute the capacity

  std::optional size = file.size_then_reset();

  if (!size) {
    error = true;
    return;
  }

  capacity = *size / elem_size;

  if (used > capacity) {
    error = true;
    return;
  }
}

template <typename T>
file_based_vector<T>& file_based_vector<T>::operator=(
    file_based_vector<T>&& other)
{
  std::swap(used, other.used);
  std::swap(capacity, other.capacity);
  std::swap(iter, other.iter);
  std::swap(capacity_chunk_size, other.capacity_chunk_size);
  std::swap(file, other.file);
  std::swap(error, other.error);
  return *this;
}

}; /* namespace dedup::util */

#endif  // BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_