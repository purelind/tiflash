// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Exception.h>
#include <Interpreters/Context.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileDataProvider.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFilePersisted.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileSchema.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileTiny.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/WriteBatchesImpl.h>
#include <Storages/DeltaMerge/convertColumnTypeHelpers.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>

#include <memory>


namespace DB
{
namespace DM
{
Columns ColumnFileTiny::readFromCache(const ColumnDefines & column_defines, size_t col_start, size_t col_end) const
{
    if (!cache)
        return {};

    Columns columns;
    const auto & colid_to_offset = schema->getColIdToOffset();
    for (size_t i = col_start; i < col_end; ++i)
    {
        const auto & cd = column_defines[i];
        if (auto it = colid_to_offset.find(cd.id); it != colid_to_offset.end())
        {
            auto col_offset = it->second;
            // Copy data from cache
            const auto & type = getDataType(cd.id);
            auto col_data = type->createColumn();
            col_data->insertRangeFrom(*cache->block.getByPosition(col_offset).column, 0, rows);
            // Cast if need
            auto col_converted = convertColumnByColumnDefineIfNeed(type, std::move(col_data), cd);
            columns.push_back(std::move(col_converted));
        }
        else
        {
            ColumnPtr column = createColumnWithDefaultValue(cd, rows);
            columns.emplace_back(std::move(column));
        }
    }
    return columns;
}

Columns ColumnFileTiny::readFromDisk(
    const IColumnFileDataProviderPtr & data_provider, //
    const ColumnDefines & column_defines,
    size_t col_start,
    size_t col_end) const
{
    const size_t num_columns_read = col_end - col_start;

    Columns columns(num_columns_read); // allocate empty columns

    std::vector<size_t> fields;
    const auto & colid_to_offset = schema->getColIdToOffset();
    for (size_t index = col_start; index < col_end; ++index)
    {
        const auto & cd = column_defines[index];
        if (auto it = colid_to_offset.find(cd.id); it != colid_to_offset.end())
        {
            auto col_index = it->second;
            fields.emplace_back(col_index);
        }
        else
        {
            // New column after ddl is not exist in this CFTiny, fill with default value
            columns[index - col_start] = createColumnWithDefaultValue(cd, rows);
        }
    }

    // All columns to be read are not exist in this CFTiny and filled with default value,
    // we can skip reading from disk
    if (fields.empty())
        return columns;

    // Read the columns from disk and apply DDL cast if need
    Page page = data_provider->readTinyData(data_page_id, fields);
    if (file_provider->isKeyspaceEncryptionEnabled())
    {
        const auto ep = EncryptionPath(std::to_string(keyspace_id), "");
        size_t data_size = page.data.size();
        // decrypt the page data in place
        char * data = page.mem_holder.get();
        file_provider->decryptPage(ep, data, data_size, data_page_id);
    }

    for (size_t index = col_start; index < col_end; ++index)
    {
        const size_t index_in_read_columns = index - col_start;
        if (columns[index_in_read_columns] != nullptr)
        {
            // the column is fill with default values.
            continue;
        }
        auto col_id = column_defines[index].id;
        auto col_index = colid_to_offset.at(col_id);
        auto data_buf = page.getFieldData(col_index);

        const auto & cd = column_defines[index];
        // Deserialize column by pack's schema
        const auto & type = getDataType(cd.id);
        auto col_data = type->createColumn();
        deserializeColumn(*col_data, type, data_buf, rows);

        columns[index_in_read_columns] = convertColumnByColumnDefineIfNeed(type, std::move(col_data), cd);
    }

    return columns;
}

void ColumnFileTiny::fillColumns(
    const IColumnFileDataProviderPtr & data_provider,
    const ColumnDefines & col_defs,
    size_t col_count,
    Columns & result) const
{
    if (result.size() >= col_count)
        return;

    size_t col_start = result.size();
    size_t col_end = col_count;

    Columns read_cols = readFromCache(col_defs, col_start, col_end);
    if (read_cols.empty())
        read_cols = readFromDisk(data_provider, col_defs, col_start, col_end);

    result.insert(result.end(), read_cols.begin(), read_cols.end());
}

ColumnFileReaderPtr ColumnFileTiny::getReader(
    const DMContext &,
    const IColumnFileDataProviderPtr & data_provider,
    const ColumnDefinesPtr & col_defs) const
{
    return std::make_shared<ColumnFileTinyReader>(*this, data_provider, col_defs);
}

void ColumnFileTiny::serializeMetadata(WriteBuffer & buf, bool save_schema) const
{
    serializeSchema(buf, save_schema ? schema->getSchema() : Block{});

    writeIntBinary(data_page_id, buf);
    writeIntBinary(rows, buf);
    writeIntBinary(bytes, buf);
}

ColumnFilePersistedPtr ColumnFileTiny::deserializeMetadata(
    const DMContext & context,
    ReadBuffer & buf,
    ColumnFileSchemaPtr & last_schema)
{
    auto schema_block = deserializeSchema(buf);
    std::shared_ptr<ColumnFileSchema> schema;

    if (!schema_block)
        schema = last_schema;
    else
    {
        schema = getSharedBlockSchemas(context)->getOrCreate(*schema_block);
        last_schema = schema;
    }

    if (unlikely(!schema))
        throw Exception("Cannot deserialize DeltaPackBlock's schema", ErrorCodes::LOGICAL_ERROR);

    PageIdU64 data_page_id;
    size_t rows, bytes;

    readIntBinary(data_page_id, buf);
    readIntBinary(rows, buf);
    readIntBinary(bytes, buf);

    return std::make_shared<ColumnFileTiny>(schema, rows, bytes, data_page_id, context);
}

std::tuple<ColumnFilePersistedPtr, BlockPtr> ColumnFileTiny::createFromCheckpoint(
    const LoggerPtr & parent_log,
    const DMContext & context,
    ReadBuffer & buf,
    UniversalPageStoragePtr temp_ps,
    const BlockPtr & last_schema,
    WriteBatches & wbs)
{
    auto schema = deserializeSchema(buf);
    if (!schema)
        schema = last_schema;
    RUNTIME_CHECK(schema != nullptr);

    PageIdU64 data_page_id;
    size_t rows, bytes;

    readIntBinary(data_page_id, buf);
    readIntBinary(rows, buf);
    readIntBinary(bytes, buf);
    auto new_cf_id = context.storage_pool->newLogPageId();
    /// Generate a new RemotePage with an entry with data location on S3
    auto remote_page_id = UniversalPageIdFormat::toFullPageId(
        UniversalPageIdFormat::toFullPrefix(context.keyspace_id, StorageType::Log, context.physical_table_id),
        data_page_id);
    // The `data_file_id` in temp_ps is lock key, we need convert it to data key before write to local ps
    auto remote_data_location = temp_ps->getCheckpointLocation(remote_page_id);
    RUNTIME_CHECK(remote_data_location.has_value());
    auto remote_data_file_lock_key_view = S3::S3FilenameView::fromKey(*remote_data_location->data_file_id);
    RUNTIME_CHECK(remote_data_file_lock_key_view.isLockFile());
    auto remote_data_file_key = remote_data_file_lock_key_view.asDataFile().toFullKey();
    PS::V3::CheckpointLocation new_remote_data_location{
        .data_file_id = std::make_shared<String>(remote_data_file_key),
        .offset_in_file = remote_data_location->offset_in_file,
        .size_in_file = remote_data_location->size_in_file,
    };
    // TODO: merge the `getEntry` and `getCheckpointLocation`
    auto entry = temp_ps->getEntry(remote_page_id);
    LOG_DEBUG(
        parent_log,
        "Write remote page to local, page_id={} remote_location={} remote_page_id={}",
        new_cf_id,
        new_remote_data_location.toDebugString(),
        remote_page_id);
    wbs.log.putRemotePage(new_cf_id, 0, entry.size, new_remote_data_location, std::move(entry.field_offsets));

    auto column_file_schema = std::make_shared<ColumnFileSchema>(*schema);
    return {
        std::make_shared<ColumnFileTiny>(column_file_schema, rows, bytes, new_cf_id, context),
        std::move(schema),
    };
}

Block ColumnFileTiny::readBlockForMinorCompaction(const PageReader & page_reader) const
{
    if (cache)
    {
        std::scoped_lock lock(cache->mutex);

        auto & cache_block = cache->block;
        MutableColumns columns = cache_block.cloneEmptyColumns();
        for (size_t i = 0; i < cache_block.columns(); ++i)
            columns[i]->insertRangeFrom(*cache_block.getByPosition(i).column, 0, rows);
        return cache_block.cloneWithColumns(std::move(columns));
    }
    else
    {
        const auto & schema_ref = schema->getSchema();
        auto page = page_reader.read(data_page_id);
        auto columns = schema_ref.cloneEmptyColumns();

        if (unlikely(columns.size() != page.fieldSize()))
            throw Exception("Column size and field size not the same");

        for (size_t index = 0; index < schema_ref.columns(); ++index)
        {
            auto data_buf = page.getFieldData(index);
            const auto & type = schema_ref.getByPosition(index).type;
            auto & column = columns[index];
            deserializeColumn(*column, type, data_buf, rows);
        }

        return schema_ref.cloneWithColumns(std::move(columns));
    }
}

ColumnFileTinyPtr ColumnFileTiny::writeColumnFile(
    const DMContext & context,
    const Block & block,
    size_t offset,
    size_t limit,
    WriteBatches & wbs,
    const CachePtr & cache)
{
    auto page_id = writeColumnFileData(context, block, offset, limit, wbs);

    auto schema = getSharedBlockSchemas(context)->getOrCreate(block);

    auto bytes = block.bytes(offset, limit);
    return std::make_shared<ColumnFileTiny>(schema, limit, bytes, page_id, context, cache);
}

PageIdU64 ColumnFileTiny::writeColumnFileData(
    const DMContext & dm_context,
    const Block & block,
    size_t offset,
    size_t limit,
    WriteBatches & wbs)
{
    auto page_id = dm_context.storage_pool->newLogPageId();

    MemoryWriteBuffer write_buf;
    PageFieldSizes col_data_sizes;
    for (const auto & col : block)
    {
        auto last_buf_size = write_buf.count();
        serializeColumn(
            write_buf,
            *col.column,
            col.type,
            offset,
            limit,
            dm_context.global_context.getSettingsRef().dt_compression_method,
            dm_context.global_context.getSettingsRef().dt_compression_level);
        size_t serialized_size = write_buf.count() - last_buf_size;
        RUNTIME_CHECK_MSG(
            serialized_size != 0,
            "try to persist a block with empty column, colname={} colid={} block={}",
            col.name,
            col.column_id,
            block.dumpJsonStructure());
        col_data_sizes.push_back(serialized_size);
    }

    auto data_size = write_buf.count();
    if (const auto & file_provider = dm_context.global_context.getFileProvider();
        file_provider->isKeyspaceEncryptionEnabled())
    {
        const auto ep = EncryptionPath(std::to_string(dm_context.keyspace_id), "");
        if (unlikely(!file_provider->isFileEncrypted(ep)))
        {
            file_provider->createEncryptionInfo(ep);
        }
        auto * data = write_buf.internalBuffer().begin();
        file_provider->encryptPage(ep, data, data_size, page_id);
    }

    auto buf = write_buf.tryGetReadBuffer();
    wbs.log.putPage(page_id, 0, buf, data_size, col_data_sizes);

    return page_id;
}

void ColumnFileTiny::removeData(WriteBatches & wbs) const
{
    wbs.removed_log.delPage(data_page_id);
}

ColumnPtr ColumnFileTinyReader::getPKColumn()
{
    tiny_file.fillColumns(data_provider, *col_defs, 1, cols_data_cache);
    return cols_data_cache[0];
}

ColumnPtr ColumnFileTinyReader::getVersionColumn()
{
    tiny_file.fillColumns(data_provider, *col_defs, 2, cols_data_cache);
    return cols_data_cache[1];
}

std::pair<size_t, size_t> ColumnFileTinyReader::readRows(
    MutableColumns & output_cols,
    size_t rows_offset,
    size_t rows_limit,
    const RowKeyRange * range)
{
    tiny_file.fillColumns(data_provider, *col_defs, output_cols.size(), cols_data_cache);

    auto & pk_col = cols_data_cache[0];
    return copyColumnsData(cols_data_cache, pk_col, output_cols, rows_offset, rows_limit, range);
}

Block ColumnFileTinyReader::readNextBlock()
{
    if (read_done)
        return {};

    Columns columns;
    tiny_file.fillColumns(data_provider, *col_defs, col_defs->size(), columns);

    read_done = true;

    return genBlock(*col_defs, columns);
}

size_t ColumnFileTinyReader::skipNextBlock()
{
    if (read_done)
        return 0;

    read_done = true;
    return tiny_file.getRows();
}

ColumnFileReaderPtr ColumnFileTinyReader::createNewReader(const ColumnDefinesPtr & new_col_defs)
{
    // Reuse the cache data.
    return std::make_shared<ColumnFileTinyReader>(tiny_file, data_provider, new_col_defs, cols_data_cache);
}

} // namespace DM
} // namespace DB
