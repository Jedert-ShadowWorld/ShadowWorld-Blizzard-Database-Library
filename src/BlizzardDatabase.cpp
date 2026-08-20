#include "BlizzardDatabase.h"
#include <cassert>
#include <optional>
#include <stdexcept>

namespace BlizzardDatabaseLib
{
    namespace
    {
        bool BuildNotAfter(const Structures::Build& lhs, const Structures::Build& rhs)
        {
            return lhs == rhs || lhs < rhs;
        }

        bool ResolveNearestPriorDefinition(DatabaseDefinition& databaseDefinition,
                                           const Structures::Build& requestedBuild,
                                           Structures::VersionDefinition& tableDefinition)
        {
            auto definitionSet = databaseDefinition.Read();
            Structures::VersionDefinitions const* bestVersion = nullptr;
            std::optional<Structures::Build> bestBuild;

            auto consider = [&](Structures::VersionDefinitions const& version,
                                Structures::Build const& candidate)
            {
                if (!BuildNotAfter(candidate, requestedBuild))
                    return;

                if (!bestBuild.has_value() || *bestBuild < candidate)
                {
                    bestBuild = candidate;
                    bestVersion = &version;
                }
            };

            for (auto const& version : definitionSet.versionDefinitions)
            {
                for (auto const& build : version.builds)
                    consider(version, build);

                for (auto const& range : version.buildRanges)
                {
                    // If the requested build is inside a range DatabaseDefinition::For
                    // would already have matched it. For a fallback, the newest safe
                    // point represented by the range is its upper endpoint.
                    consider(version, range.maxBuild());
                }
            }

            if (!bestVersion)
                return false;

            tableDefinition = Structures::VersionDefinition();
            tableDefinition.columnDefinitions = definitionSet.columnDefinitions;
            tableDefinition.versionDefinitions = *bestVersion;
            tableDefinition.initializeRowDefinition();

            for (int i = 0; i < static_cast<int>(tableDefinition.RowDefinition.ColumnDefinitions.size()); ++i)
            {
                auto const& internalDefinition = tableDefinition.versionDefinitions.definitions[i];
                if (!internalDefinition.isID)
                    continue;

                if (!tableDefinition.hasId)
                {
                    tableDefinition.hasId = true;
                    tableDefinition.idColumnIndex = i;
                }
            }

            if (bestBuild.has_value())
            {
                std::cout << "Using nearest prior DB2 schema "
                          << bestBuild->expansion() << '.'
                          << bestBuild->major() << '.'
                          << bestBuild->minor() << '.'
                          << bestBuild->buildId()
                          << " for requested "
                          << requestedBuild.expansion() << '.'
                          << requestedBuild.major() << '.'
                          << requestedBuild.minor() << '.'
                          << requestedBuild.buildId()
                          << std::endl;
            }

            return true;
        }

        bool ResolveTableDefinition(DatabaseDefinition& databaseDefinition,
                                    const Structures::Build& requestedBuild,
                                    Structures::VersionDefinition& tableDefinition)
        {
            if (databaseDefinition.For(requestedBuild, tableDefinition))
                return true;

            // Shadowlands 9.2.7.45745 is absent from several DBD files even when
            // those tables have a valid late-Shadowlands (or unchanged earlier)
            // layout. Resolve the closest layout not newer than the client build
            // instead of requiring one hard-coded build number for every table.
            if (requestedBuild.expansion() == 9 && requestedBuild.major() == 2 && requestedBuild.minor() == 7)
                return ResolveNearestPriorDefinition(databaseDefinition, requestedBuild, tableDefinition);

            return false;
        }
    }

    BlizzardDatabase::BlizzardDatabase(const std::string& databaseDefinitionDirectory, const Structures::Build& build)
    : _databaseDefinitionFilesLocation(databaseDefinitionDirectory)
    , _build(build)
    {
        _loadedTables = std::map<std::string, std::shared_ptr<BlizzardDatabaseTable>>();
        _blizzardTableReaderFactory = Reader::BlizzardTableReaderFactory();
        _table_definitions = std::map<std::string, Structures::VersionDefinition>();
    }

    // must use as a ref, copies not allowed
    BlizzardDatabaseTable& BlizzardDatabase::LoadTable(const std::string& tableName,
       std::function<std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>(std::string const&)> file_callback)
    {
        if (_loadedTables.contains(tableName))
        {
            std::cout << "Table Already Loaded" << std::endl;
            return *_loadedTables[tableName];
        }

        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableDefinition);
        tableDefinition.tableName = tableName;

        if (!tableFound)
            throw std::runtime_error("No DB2 schema definition for table " + tableName + " and requested build");

        auto fileName = "DBFilesClient\\" + tableName + ".dbc";
        const Structures::Build& dbcCutoffBuild = Structures::Build("7.0.3.21287");
        if (_build > dbcCutoffBuild)
            fileName = "DBFilesClient\\" + tableName + ".db2";

        auto fileStream = file_callback(fileName);
        auto streamReader = std::make_shared<Stream::StreamReader>(fileStream);
        auto fileFormatIdentifier = streamReader->ReadString(4);

        auto tableReader = _blizzardTableReaderFactory.For(streamReader, tableDefinition, fileFormatIdentifier);
        auto constructedTable = std::make_shared<BlizzardDatabaseTable>(tableReader, tableName);
        constructedTable->LoadTableStructure();

        _loadedTables.emplace(tableName, constructedTable);
        return *_loadedTables[tableName];
    }

    bool BlizzardDatabase::SaveTable(const std::string& outputDirectory, const std::string& tableName, std::vector<Structures::BlizzardDatabaseRow>& rows)
    {
        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableDefinition);

        if (!tableFound)
            throw std::runtime_error("No database schema definition for table " + tableName + " and requested build");

        auto filePath = std::filesystem::path(outputDirectory) / (tableName + ".dbc");
        auto outputStream = std::ofstream(filePath, std::ios::out | std::ios::binary);
        auto fileWriter = Writer::WDBCTableWriter(outputStream, tableDefinition);
        return fileWriter.Write(rows);
    }

    void BlizzardDatabase::UnloadTable(const std::string& tableName)
    {
        if (!_loadedTables.contains(tableName))
        {
            std::cout << "Table Not Loaded" << std::endl;
            return;
        }

        auto& table = _loadedTables.at(tableName);
        table.reset();
        _loadedTables.erase(tableName);
    }

    Structures::VersionDefinition& BlizzardDatabase::TableDefinition(const std::string& tableName)
    {
      if (_table_definitions.contains(tableName))
        return _table_definitions[tableName];

      auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
        (tableName + ".dbd");

      auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
      auto tableVersionDefinition = Structures::VersionDefinition();
      auto tableFound = ResolveTableDefinition(databaseDefinition, _build, tableVersionDefinition);

      if (!tableFound)
        throw std::runtime_error("No database schema definition for table " + tableName + " and requested build");

      tableVersionDefinition.tableName = tableName;
      _table_definitions.emplace(tableName, tableVersionDefinition);
      return _table_definitions[tableName];
    }

    Structures::BlizzardDatabaseRowDefinition& BlizzardDatabase::TableRecordDefinition(const std::string& tableName)
    {
      return TableDefinition(tableName).RowDefinition;
    }
}
