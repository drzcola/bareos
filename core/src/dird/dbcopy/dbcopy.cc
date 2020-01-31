/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2020-2020 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "include/bareos.h"
#include "cats/cats.h"
#include "cats/cats_backends.h"
#include "cats/sql_pooling.h"
#include "dird/get_database_connection.h"
#include "dird/dird_conf.h"
#include "dird/dird_globals.h"
#include "dird/jcr_private.h"
#include "dird/job.h"
#include "dird/dbcopy/database_connection.h"
#include "dird/dbcopy/database_export.h"
#include "dird/dbcopy/database_export_postgresql.h"
#include "dird/dbcopy/database_import.h"
#include "include/make_unique.h"
#include "lib/parse_conf.h"
#include "lib/util.h"

#include <iostream>
#include <array>

#if !defined HAVE_DYNAMIC_CATS_BACKENDS
#error "NOT DEFINED: HAVE_DYNAMIC_CATS_BACKENDS"
#endif

namespace directordaemon {
bool DoReloadConfig() { return false; }
}  // namespace directordaemon

class DbCopy {
 public:
  explicit DbCopy(int argc, char** argv)
  {
    InitMsg(nullptr, nullptr);
    SetWorkingDir();
    cl.ParseCommandLine(argc, argv);

    if (cl.source_db_resource_name != "mysql")
      throw std::runtime_error("source database is not mysql");
    if (cl.destination_db_resource_name != "postgresql")
      throw std::runtime_error("destination database is not postgresql");

    ParseConfig();
    std::cout << "Copying tables from \"" << cl.source_db_resource_name
              << "\" to \"" << cl.destination_db_resource_name << "\""
              << std::endl;
    ConnectToDatabases();
  }

  void DoDatabaseCopy()
  {
    std::cout << "gathering info about source catalog \""
              << cl.source_db_resource_name << "\"..." << std::endl;
    std::unique_ptr<DatabaseImport> imp(
        DatabaseImport::Create(*source_db_, cl.maximum_number_of_rows));

    std::cout << "gathering info about destination catalog \""
              << cl.destination_db_resource_name << "\"..." << std::endl;
    std::unique_ptr<DatabaseExport> exp(
        DatabaseExport::Create(*destination_db_, cl.empty_destination_tables));

    std::cout << "copying tables..." << std::endl;
    imp->ExportTo(*exp);
#if 0
    if (cl.compare_all_rows) { imp->CompareWith(*exp); }
#endif
  }

 private:
  void SetWorkingDir()
  {
    if (getcwd(current_working_directory_.data(),
               current_working_directory_.size()) == nullptr) {
      throw std::runtime_error(
          "Could not determine current working directory.");
    }
    SetWorkingDirectory(current_working_directory_.data());
  }

  void ParseConfig()
  {
    directordaemon::my_config =
        directordaemon::InitDirConfig(cl.configpath_.c_str(), M_ERROR_TERM);

    my_config_.reset(directordaemon::my_config);

    if (!directordaemon::my_config->ParseConfig()) {
      throw std::runtime_error("Error when loading config.");
    }

    directordaemon::me = static_cast<directordaemon::DirectorResource*>(
        my_config->GetNextRes(directordaemon::R_DIRECTOR, nullptr));

    if (!directordaemon::me) {
      throw std::runtime_error("Could not find director resource.");
    }

    DbSetBackendDirs(std::move(directordaemon::me->backend_directories));
  }

  void ConnectToDatabases()
  {
    try {
      source_db_ = std::make_unique<DatabaseConnection>(
          cl.source_db_resource_name, directordaemon::my_config);

      destination_db_ = std::make_unique<DatabaseConnection>(
          cl.destination_db_resource_name, directordaemon::my_config);

    } catch (const std::runtime_error& e) {
      throw e;
    }
  }

  class CommandLineParser {
    friend class DbCopy;

   public:
    void ParseCommandLine(int argc, char** argv)
    {
      char c{};
      bool options_error{false};
      int argument_count{};

      while ((c = getopt(argc, argv, "cl:?")) != -1 && !options_error) {
        switch (c) {
          case 'c':
            configpath_ = optarg;
            argument_count += 2;
            break;
#if 0
          case 'd':
            empty_destination_tables = true;
            ++argument_count;
            break;
          case 'e':
            compare_all_rows = true;
            ++argument_count;
            break;
#endif
          case 'l':
            try {
              maximum_number_of_rows = std::atoi(optarg);
            } catch (...) {
              throw std::runtime_error("Wrong argument for 'l'");
            }
            argument_count += 2;
            break;
          case '?':
          default:
            options_error = true;
            break;
        }
      }

      argument_count += 3;

      if (options_error || argc != argument_count) {
        usage();
        throw std::runtime_error(std::string());
      }
      source_db_resource_name = argv[argument_count - 2];
      destination_db_resource_name = argv[argument_count - 1];
    }

   private:
    std::string configpath_{"/etc/bareos"};
    std::string source_db_resource_name, destination_db_resource_name;
    bool empty_destination_tables{false};
    bool compare_all_rows{false};
    std::size_t maximum_number_of_rows;

    void usage() noexcept
    {
      kBareosVersionStrings.PrintCopyright(stderr, 2020);

      fprintf(stderr,
              _("Usage: bareos-dbcopy [options] Source Destination\n"
                "        -c <path>   use <path> as configuration file or "
                "directory\n"
                "        -?          print this message.\n"
                "\n"));
    }
  };  // class CommandLineParser

 public:
  ~DbCopy() = default;
  DbCopy(const DbCopy& other) = delete;
  DbCopy(const DbCopy&& other) = delete;
  DbCopy& operator=(const DbCopy& rhs) = delete;
  DbCopy& operator=(const DbCopy&& rhs) = delete;

 private:
  CommandLineParser cl;
  std::unique_ptr<DatabaseConnection> source_db_;
  std::unique_ptr<DatabaseConnection> destination_db_;
  std::unique_ptr<ConfigurationParser> my_config_;
  std::array<char, 1024> current_working_directory_;
};

class Cleanup {
 public:
  ~Cleanup()
  {
    DbSqlPoolDestroy();
    DbFlushBackends();
  }
};

int main(int argc, char** argv)
{
  Cleanup c;

  try {
    DbCopy dbcopy(argc, argv);
    dbcopy.DoDatabaseCopy();
  } catch (const std::runtime_error& e) {
    std::string errstring{e.what()};
    if (!errstring.empty()) {
      std::cerr << std::endl << std::endl << e.what() << std::endl;
    }
    return 1;
  }
  std::cout << "database copy completed successfully" << std::endl;
  return 0;
}
