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

#include "dedup_volume.h"

namespace dedup {
constexpr config::loaded_general_info my_general_info = {
    .block_header_size = sizeof(bareos_block_header),
    .record_header_size = sizeof(bareos_record_header),
    .dedup_block_header_size = sizeof(block_header),
    .dedup_record_header_size = sizeof(record_header),
};

void volume::write_current_config()
{
  std::vector<config::loaded_block_section> blocksections;
  std::vector<config::loaded_record_section> recordsections;
  std::vector<config::loaded_data_section> datasections;
  std::vector<config::loaded_unfinished_record> unfinished;

  for (auto& blockfile : config.blockfiles) {
    blocksections.emplace_back(blockfile.begin(), blockfile.end(),
                               blockfile.path());
  }
  for (auto& recordfile : config.recordfiles) {
    recordsections.emplace_back(recordfile.begin(), recordfile.end(),
                                recordfile.path());
  }
  for (auto& datafile : config.datafiles) {
    datasections.emplace_back(datafile.index(), datafile.blocksize(),
                              datafile.path(), datafile.end());
  }

  for (auto& [record, loc] : unfinished_records) {
    unfinished.emplace_back(record.VolSessionId, record.VolSessionTime,
                            record.FileIndex, record.Stream, loc.file_index,
                            loc.current, loc.end - loc.current);
  }


  std::vector<std::byte> bytes = config::to_bytes(
      my_general_info, datasections, recordsections, blocksections, unfinished);
  if (ftruncate(configfile.fd.get(), 0) != 0) {
    error = true;
  } else if (::lseek(configfile.fd.get(), 0, SEEK_SET) != 0) {
    error = true;
  } else if (write(configfile.fd.get(), &bytes.front(), bytes.size())
             != static_cast<ssize_t>(bytes.size())) {
    error = true;
  }
}

bool volume::load_config()
{
  auto config_end = lseek(configfile.fd.get(), 0, SEEK_END);
  auto config_start = lseek(configfile.fd.get(), 0, SEEK_SET);

  if (config_start != 0 || config_start > config_end) {
    // error: cannot determine config file size
    return false;
  }

  std::vector<std::byte> bytes(config_end - config_start);

  if (read(configfile.fd.get(), &bytes.front(), bytes.size())
      != static_cast<ssize_t>(bytes.size())) {
    // error: cannot read config file
    return false;
  }

  std::optional loaded_config = config::from_bytes(bytes);
  if (!loaded_config) { return false; }

  // at the moment we only support configurations that have
  // exactly one block and one record file.
  // This might change in the future
  if (loaded_config->blockfiles.size() != 1) {
    // error: to many/few block files
    return false;
  }

  if (loaded_config->recordfiles.size() != 1) {
    // error: to many/few record files
    return false;
  }

  if (loaded_config->info.block_header_size != sizeof(bareos_block_header)
      || loaded_config->info.dedup_block_header_size != sizeof(block_header)) {
    // error: bad block header size
    return false;
  }

  if (loaded_config->info.record_header_size != sizeof(bareos_record_header)
      || loaded_config->info.dedup_record_header_size
             != sizeof(record_header)) {
    // error: bad record header size
    return false;
  }

  for (auto& rec : loaded_config->unfinished) {
    // FIXME: check wether this write_loc makes any sense!
    record unfinished_record{rec.VolSessionId, rec.VolSessionTime,
                             rec.FileIndex, rec.Stream};
    write_loc loc{rec.DataIdx, rec.file_offset, rec.file_offset + rec.size};
    auto [_, inserted] = unfinished_records.emplace(unfinished_record, loc);
    if (!inserted) {
      // error: bad unfinished record
      return false;
    }
  }

  config = volume_config(std::move(loaded_config.value()));
  return true;
}
} /* namespace dedup */