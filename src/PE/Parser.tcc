/* Copyright 2017 - 2022 R. Thomas
 * Copyright 2017 - 2022 Quarkslab
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <memory>

#include "logging.hpp"

#include "LIEF/BinaryStream/VectorStream.hpp"
#include "LIEF/PE/LoadConfigurations.hpp"
#include "LIEF/PE/Parser.hpp"
#include "LIEF/PE/Binary.hpp"
#include "LIEF/PE/DataDirectory.hpp"
#include "LIEF/PE/EnumToString.hpp"
#include "LIEF/PE/Section.hpp"
#include "LIEF/PE/ImportEntry.hpp"

#include "PE/Structures.hpp"
#include "LoadConfigurations/LoadConfigurations.tcc"
#include "LIEF/PE/Parser.hpp"


namespace LIEF {
namespace PE {

template<typename PE_T>
ok_error_t Parser::parse() {

  if (!parse_headers<PE_T>()) {
    return make_error_code(lief_errors::parsing_error);
  }

  LIEF_DEBUG("[+] Processing DOS stub & Rich header");

  if (!parse_dos_stub()) {
    LIEF_WARN("Fail to parse the DOS Stub");
  }

  if (!parse_rich_header()) {
    LIEF_WARN("Fail to parse the rich header");
  }

  LIEF_DEBUG("[+] Processing sections");

  try {
    if (!parse_sections()) {
      LIEF_WARN("Fail to parse the sections");
    }
  } catch (const corrupted& e) {
    LIEF_WARN("{}", e.what());
  }

  LIEF_DEBUG("[+] Processing data directories");

  try {
    if (!parse_data_directories<PE_T>()) {
      LIEF_WARN("Fail to parse the data directories");
    }
  } catch (const exception& e) {
    LIEF_WARN("{}", e.what());
  }

  try {
    if (!parse_symbols()) {
      LIEF_WARN("Fail to parse the symbols");
    }
  } catch (const corrupted& e) {
    LIEF_WARN("{}", e.what());
  }

  if (!parse_overlay()) {
    LIEF_WARN("Fail to parse the overlay");
  }
  return ok();
}

template<typename PE_T>
ok_error_t Parser::parse_headers() {
  using pe_optional_header = typename PE_T::pe_optional_header;

  auto dos_hdr = stream_->peek<details::pe_dos_header>(0);
  if (!dos_hdr) {
    LIEF_ERR("Can't read the Dos Header");
    return dos_hdr.error();
  }

  binary_->dos_header_ = *dos_hdr;
  const uint64_t addr_new_exe = binary_->dos_header().addressof_new_exeheader();

  {
    auto pe_header = stream_->peek<details::pe_header>(addr_new_exe);
    if (!pe_header) {
      LIEF_ERR("Can't read the PE header");
      return pe_header.error();
    }
    binary_->header_ = *pe_header;
  }

  {
    const uint64_t offset = addr_new_exe + sizeof(details::pe_header);
    auto opt_header = stream_->peek<pe_optional_header>(offset);
    if (!opt_header) {
      LIEF_ERR("Can't read the optional header");
      return opt_header.error();
    }
    binary_->optional_header_ = *opt_header;
  }

  return ok();
}

template<typename PE_T>
ok_error_t Parser::parse_data_directories() {
  using pe_optional_header = typename PE_T::pe_optional_header;
  const uint32_t directories_offset = binary_->dos_header().addressof_new_exeheader() +
                                      sizeof(details::pe_header) + sizeof(pe_optional_header);
  const auto nbof_datadir = static_cast<uint32_t>(DATA_DIRECTORY::NUM_DATA_DIRECTORIES);
  stream_->setpos(directories_offset);

  // WARNING: The PE specifications require that the data directory table ends
  // with a null entry (RVA / Size, set to 0).
  //
  // Nevertheless it seems that this requirement is not enforced by the PE loader.
  // The binary bc203f2b6a928f1457e9ca99456747bcb7adbbfff789d1c47e9479aac11598af contains a non-null final
  // data directory (watermarking?)
  for (size_t i = 0; i < nbof_datadir; ++i) {
    auto data_dir = stream_->read<details::pe_data_directory>();
    if (!data_dir) {
      LIEF_ERR("Can't read data directory at #{}", i);
      return make_error_code(lief_errors::read_error);
    }
    auto directory = std::make_unique<DataDirectory>(*data_dir, static_cast<DATA_DIRECTORY>(i));
    if (directory->RVA() > 0) {
      // Data directory is not always associated with section
      const uint64_t offset = binary_->rva_to_offset(directory->RVA());
      directory->section_   = binary_->section_from_offset(offset);
      if (directory->section_ == nullptr) {
        LIEF_WARN("Unable to find the section associated with {}", to_string(static_cast<DATA_DIRECTORY>(i)));
      }
    }
    binary_->data_directories_.push_back(std::move(directory));
  }

  try {
    // Import Table
    DataDirectory& import_data_dir = binary_->data_directory(DATA_DIRECTORY::IMPORT_TABLE);
    if (import_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing Import Table");
      if (import_data_dir.has_section()) {
        import_data_dir.section()->add_type(PE_SECTION_TYPES::IMPORT);
      }
      parse_import_table<PE_T>();
    }
  } catch (const exception& e) {
    LIEF_WARN("{}", e.what());
  }

  // Exports
  if (binary_->data_directory(DATA_DIRECTORY::EXPORT_TABLE).RVA() > 0) {
    LIEF_DEBUG("[+] Processing Exports");
    try {
      parse_exports();
    } catch (const exception& e) {
      LIEF_WARN("{}", e.what());
    }
  }

  // Signature
  if (binary_->data_directory(DATA_DIRECTORY::CERTIFICATE_TABLE).RVA() > 0) {
    try {
      parse_signature();
    } catch (const exception& e) {
      LIEF_WARN("{}", e.what());
    }
  }

  {
    DataDirectory& tls_data_dir = binary_->data_directory(DATA_DIRECTORY::TLS_TABLE);
    if (tls_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing TLS");
      if (tls_data_dir.has_section()) {
        tls_data_dir.section()->add_type(PE_SECTION_TYPES::TLS);
      }
      try {
        parse_tls<PE_T>();
      } catch (const exception& e) {
        LIEF_WARN("{}", e.what());
      }
    }
  }

  {
    DataDirectory& load_config_data_dir = binary_->data_directory(DATA_DIRECTORY::LOAD_CONFIG_TABLE);
    if (load_config_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing LoadConfiguration");
      if (load_config_data_dir.has_section()) {
        load_config_data_dir.section()->add_type(PE_SECTION_TYPES::LOAD_CONFIG);
      }
      try {
        parse_load_config<PE_T>();
      } catch (const exception& e) {
        LIEF_WARN("{}", e.what());
      }
    }
  }

  {
    DataDirectory& reloc_data_dir = binary_->data_directory(DATA_DIRECTORY::BASE_RELOCATION_TABLE);
    if (reloc_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing Relocations");
      if (reloc_data_dir.has_section()) {
        reloc_data_dir.section()->add_type(PE_SECTION_TYPES::RELOCATION);
      }
      try {
        parse_relocations();
      } catch (const exception& e) {
        LIEF_WARN("{}", e.what());
      }
    }
  }

  {
    DataDirectory& debug_data_dir = binary_->data_directory(DATA_DIRECTORY::DEBUG);
    if (debug_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing Debug");
      if (debug_data_dir.has_section()) {
        debug_data_dir.section()->add_type(PE_SECTION_TYPES::DEBUG);
      }
      try {
        parse_debug();
      } catch (const exception& e) {
        LIEF_WARN("{}", e.what());
      }
    }
  }
  {
    DataDirectory& res_data_dir = binary_->data_directory(DATA_DIRECTORY::RESOURCE_TABLE);
    if (res_data_dir.RVA() > 0) {
      LIEF_DEBUG("Processing Resources");
      if (res_data_dir.has_section()) {
        res_data_dir.section()->add_type(PE_SECTION_TYPES::RESOURCE);
      }
      try {
        parse_resources();
      } catch (const exception& e) {
        LIEF_WARN("{}", e.what());
      }
    }
  }

  return ok();
}

template<typename PE_T>
ok_error_t Parser::parse_import_table() {
  using uint__ = typename PE_T::uint;

  const uint32_t import_rva    = binary_->data_directory(DATA_DIRECTORY::IMPORT_TABLE).RVA();
  const uint64_t import_offset = binary_->rva_to_offset(import_rva);
  stream_->setpos(import_offset);

  while (auto raw_imp = stream_->read<details::pe_import>()) {
    Import import           = *raw_imp;
    import.directory_       = &(binary_->data_directory(DATA_DIRECTORY::IMPORT_TABLE));
    import.iat_directory_   = &(binary_->data_directory(DATA_DIRECTORY::IAT));
    import.type_            = type_;

    if (import.name_RVA_ == 0) {
      LIEF_DEBUG("Name's RVA is null");
      break;
    }

    // Offset to the Import (Library) name
    const uint64_t offset_name = binary_->rva_to_offset(import.name_RVA_);
    auto res_name = stream_->peek_string_at(offset_name);
    if (!res_name) {
      LIEF_ERR("Can't read the import name (offset: 0x{:x})", offset_name);
      continue;
    }

    import.name_ = std::move(*res_name);

    // We assume that a DLL name should be at least 4 length size and "printable
    const std::string& imp_name = import.name();
    if (!is_valid_dll_name(imp_name)) {
      if (!imp_name.empty()) {
        LIEF_WARN("'{}' is not a valid import name and will be discarded", imp_name);
        continue;
      }
      continue; // skip
    }

    // Offset to import lookup table
    uint64_t LT_offset = import.import_lookup_table_rva() > 0 ?
                         binary_->rva_to_offset(import.import_lookup_table_rva()) :
                         0;


    // Offset to the import address table
    uint64_t IAT_offset = import.import_address_table_rva() > 0 ?
                          binary_->rva_to_offset(import.import_address_table_rva()) :
                          0;

    uint__ IAT = 0;
    uint__ table = 0;

    if (IAT_offset > 0) {
      auto res_iat = stream_->peek<uint__>(IAT_offset);
      if (res_iat) {
        IAT   = *res_iat;
        table = IAT;
        IAT_offset += sizeof(uint__);
      }
    }

    if (LT_offset > 0) {
      auto res_lt = stream_->peek<uint__>(LT_offset);
      if (res_lt) {
        table      = *res_lt;
        LT_offset += sizeof(uint__);
      }
    }

    size_t idx = 0;

    while (table != 0 || IAT != 0) {
      ImportEntry entry;
      entry.iat_value_ = IAT;
      entry.data_      = table > 0 ? table : IAT; // In some cases, ILT can be corrupted
      entry.type_      = type_;
      entry.rva_       = import.import_address_table_RVA_ + sizeof(uint__) * (idx++);

      if (!entry.is_ordinal()) {
        const size_t hint_off = binary_->rva_to_offset(entry.hint_name_rva());
        const size_t name_off = hint_off + sizeof(uint16_t);
        auto res_entry_name = stream_->peek_string_at(name_off);
        if (res_entry_name) {
          entry.name_ = std::move(*res_entry_name);
        } else {
          LIEF_ERR("Can't read import entry name");
        }
        entry.hint_ = static_cast<uint16_t>(hint_off);

        // Check that the import name is valid
        if (is_valid_import_name(entry.name())) {
          import.entries_.push_back(std::move(entry));
        } else if (!entry.name().empty()){
          LIEF_INFO("'{}' is an invalid import name and will be discarded", entry.name());
        }

      } else {
        import.entries_.push_back(std::move(entry));
      }

      if (IAT_offset > 0) {
        if (auto iat = stream_->peek<uint__>(IAT_offset)) {
          IAT = *iat;
          IAT_offset += sizeof(uint__);
        } else {
          LIEF_ERR("Can't read the IAT value at 0x{:x}", IAT_offset);
          IAT = 0;
        }
      } else {
        IAT = 0;
      }

      if (LT_offset > 0) {
        if (auto lt = stream_->peek<uint__>(LT_offset)) {
          table = *lt;
          LT_offset += sizeof(uint__);
        } else {
          LIEF_ERR("Can't read the Lookup Table value at 0x{:x}", LT_offset);
          table = 0;
        }
      } else {
        table = 0;
      }
    }
    binary_->imports_.push_back(std::move(import));
  }

  binary_->has_imports_ = !binary_->imports_.empty();
  return ok();
}

template<typename PE_T>
ok_error_t Parser::parse_tls() {
  using pe_tls = typename PE_T::pe_tls;
  using uint__ = typename PE_T::uint;

  LIEF_DEBUG("[+] Parsing TLS");

  const uint32_t tls_rva = binary_->data_directory(DATA_DIRECTORY::TLS_TABLE).RVA();
  const uint64_t offset  = binary_->rva_to_offset(tls_rva);

  stream_->setpos(offset);

  const auto tls_header = stream_->read<pe_tls>();

  if (!tls_header) {
    return make_error_code(lief_errors::read_error);
  }

  binary_->tls_ = *tls_header;
  TLS& tls = binary_->tls_;

  const uint64_t imagebase = binary_->optional_header().imagebase();

  if (tls_header->RawDataStartVA >= imagebase && tls_header->RawDataEndVA > tls_header->RawDataStartVA) {
    const uint64_t start_data_rva = tls_header->RawDataStartVA - imagebase;
    const uint64_t stop_data_rva  = tls_header->RawDataEndVA - imagebase;

    const uint__ start_template_offset  = binary_->rva_to_offset(start_data_rva);
    const uint__ end_template_offset    = binary_->rva_to_offset(stop_data_rva);

    const size_t size_to_read = end_template_offset - start_template_offset;

    if (size_to_read > Parser::MAX_DATA_SIZE) {
      LIEF_DEBUG("TLS's template is too large!");
    } else {
      if (!stream_->peek_data(tls.data_template_, start_template_offset, size_to_read)) {
        LIEF_WARN("TLS's template corrupted");
      }
    }
  }

  if (tls.addressof_callbacks() > imagebase) {
    uint64_t callbacks_offset = binary_->rva_to_offset(tls.addressof_callbacks() - imagebase);
    stream_->setpos(callbacks_offset);
    size_t count = 0;
    while (count++ < Parser::MAX_TLS_CALLBACKS) {
      auto res_callback_rva = stream_->read<uint__>();
      if (!res_callback_rva) {
        break;
      }

      auto callback_rva = *res_callback_rva;

      if (static_cast<uint32_t>(callback_rva) == 0) {
        break;
      }
      tls.callbacks_.push_back(callback_rva);
    }
  }

  tls.directory_ = &(binary_->data_directory(DATA_DIRECTORY::TLS_TABLE));
  tls.section_   = tls.directory_->section();

  binary_->has_tls_ = true;
  return ok();
}


template<typename PE_T>
ok_error_t Parser::parse_load_config() {
  using load_configuration_t    = typename PE_T::load_configuration_t;
  using load_configuration_v0_t = typename PE_T::load_configuration_v0_t;
  using load_configuration_v1_t = typename PE_T::load_configuration_v1_t;
  using load_configuration_v2_t = typename PE_T::load_configuration_v2_t;
  using load_configuration_v3_t = typename PE_T::load_configuration_v3_t;
  using load_configuration_v4_t = typename PE_T::load_configuration_v4_t;
  using load_configuration_v5_t = typename PE_T::load_configuration_v5_t;
  using load_configuration_v6_t = typename PE_T::load_configuration_v6_t;
  using load_configuration_v7_t = typename PE_T::load_configuration_v7_t;

  LIEF_DEBUG("[+] Parsing Load Config");

  const uint32_t ldc_rva = binary_->data_directory(DATA_DIRECTORY::LOAD_CONFIG_TABLE).RVA();
  const uint64_t offset  = binary_->rva_to_offset(ldc_rva);

  const auto size = stream_->peek<uint32_t>(offset);
  if (!size) {
    return make_error_code(lief_errors::read_error);
  }
  size_t current_size = 0;
  WIN_VERSION version_found = WIN_VERSION::WIN_UNKNOWN;

  for (const auto& p : PE_T::load_configuration_sizes) {
    if (p.second > current_size && p.second <= *size) {
      std::tie(version_found, current_size) = p;
    }
  }

  LIEF_DEBUG("Version found: {} (size: 0x{:x})", to_string(version_found), *size);
  std::unique_ptr<LoadConfiguration> ld_conf;

  switch (version_found) {

    case WIN_VERSION::WIN_SEH:
      {
        const auto header = stream_->peek<load_configuration_v0_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV0>(*header);
        break;
      }

    case WIN_VERSION::WIN8_1:
      {
        const auto header = stream_->peek<load_configuration_v1_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV1>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_9879:
      {
        const auto header = stream_->peek<load_configuration_v2_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV2>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_14286:
      {
        const auto header = stream_->peek<load_configuration_v3_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV3>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_14383:
      {
        const auto header = stream_->peek<load_configuration_v4_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV4>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_14901:
      {
        const auto header = stream_->peek<load_configuration_v5_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV5>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_15002:
      {
        const auto header = stream_->peek<load_configuration_v6_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV6>(*header);
        break;
      }

    case WIN_VERSION::WIN10_0_16237:
      {
        const auto header = stream_->peek<load_configuration_v7_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfigurationV7>(*header);
        break;
      }

    case WIN_VERSION::WIN_UNKNOWN:
    default:
      {
        const auto header = stream_->peek<load_configuration_t>(offset);
        if (!header) {
          break;
        }
        ld_conf = std::make_unique<LoadConfiguration>(*header);
      }
  }

  binary_->has_configuration_  = static_cast<bool>(ld_conf);
  binary_->load_configuration_ = std::move(ld_conf);
  return ok();
}

}
}
