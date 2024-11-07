/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
// Kern Sibbald, 2000
/**
 * @file
 * Define Message Types for BAREOS
 */

#ifndef BAREOS_LIB_SOURCE_LOCATION_H_
#define BAREOS_LIB_SOURCE_LOCATION_H_

#if defined(HAVE_SOURCE_LOCATION)
#  include <source_location>
namespace brs {
using source_location = std::source_location;
};
#elif defined(HAVE_BUILTIN_LOCATION)
#  include <cstdint>
namespace brs {
struct source_location {
  constexpr static source_location current(const char* function
                                           = __builtin_FUNCTION(),
                                           const char* file = __builtin_FILE(),
                                           std::uint_least32_t line
                                           = __builtin_LINE()) noexcept
  {
    return source_location{function, file, line};
  }

  const char* file_name() const noexcept { return file_; }
  const char* function_name() const noexcept { return function_; }
  std::uint_least32_t line() const noexcept { return line_; }
  std::uint_least32_t column() const noexcept { return 0; }

  const char* function_{"unset"};
  const char* file_{"unset"};
  std::uint_least32_t line_{0};
};
}  // namespace brs
#else
#  include <cstdint>
namespace brs {
struct source_location {
  constexpr static source_location current() noexcept
  {
    return source_location{};
  }

  const char* file_name() const noexcept { return "unknown"; }
  const char* function_name() const noexcept { return "unknown"; }
  std::uint_least32_t line() const noexcept { return 0; }
  std::uint_least32_t column() const noexcept { return 0; }
};
}  // namespace brs

#endif

#endif  // BAREOS_LIB_SOURCE_LOCATION_H_
