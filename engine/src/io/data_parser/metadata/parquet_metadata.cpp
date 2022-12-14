//
// Created by aocsa on 12/9/19.
//

#ifndef BLAZINGDB_RAL_SRC_IO_DATA_PARSER_METADATA_PARQUET_METADATA_CPP_H_
#define BLAZINGDB_RAL_SRC_IO_DATA_PARSER_METADATA_PARQUET_METADATA_CPP_H_

#include "parquet_metadata.h"
#include "ExceptionHandling/BlazingThread.h"
#include "utilities/CommonOperations.h"
#include <cudf/column/column_factories.hpp>

void set_min_max(
	std::vector<std::vector<int64_t>> &minmax_metadata_table,
	int col_index, parquet::Type::type physical,
	parquet::ConvertedType::type logical,
	std::shared_ptr<parquet::Statistics> &statistics) {

	int64_t dummy = 0;
	minmax_metadata_table[col_index].push_back(dummy);
	minmax_metadata_table[col_index + 1].push_back(dummy);

	bool set_by_logical = false;
	switch (logical) {
	case parquet::ConvertedType::type::UINT_8:
	case parquet::ConvertedType::type::INT_8:
	case parquet::ConvertedType::type::UINT_16:
	case parquet::ConvertedType::type::INT_16:
	case parquet::ConvertedType::type::DATE:
		physical = parquet::Type::type::INT32;
		break;
	case parquet::ConvertedType::type::TIMESTAMP_MILLIS: {
		auto convertedStats = std::static_pointer_cast<parquet::Int64Statistics>(statistics);
		int64_t min = statistics->HasMinMax() ? convertedStats->min() : 0;     
		int64_t max = statistics->HasMinMax() ? convertedStats->max() : 9223286400; // 04/11/2262 in ms
		minmax_metadata_table[col_index].back() = min;
		minmax_metadata_table[col_index + 1].back() = max;
		set_by_logical = true;
		break;
	}
	case parquet::ConvertedType::type::TIMESTAMP_MICROS: {
		auto convertedStats = std::static_pointer_cast<parquet::Int64Statistics>(statistics);
		int64_t min = statistics->HasMinMax() ? convertedStats->min() : 0;
		int64_t max = statistics->HasMinMax() ? convertedStats->max() : 9223286400000; // 04/11/2262 in us
		minmax_metadata_table[col_index].back() = min;
		minmax_metadata_table[col_index + 1].back() = max;
		set_by_logical = true;
		break;
	}
	default:
		break;
	}

	if (!set_by_logical){
		// Physical storage type supported by Parquet; controls the on-disk storage
		// format in combination with the encoding type.
		switch (physical) {
		case parquet::Type::type::BOOLEAN: {
			auto convertedStats = std::static_pointer_cast<parquet::BoolStatistics>(statistics);
			auto min = statistics->HasMinMax() ? convertedStats->min() : 0;
			auto max = statistics->HasMinMax() ? convertedStats->max() : 1;
			minmax_metadata_table[col_index].back() = min;
			minmax_metadata_table[col_index + 1].back() = max;
			break;
		}
		case parquet::Type::type::INT32: {
			auto convertedStats = std::static_pointer_cast<parquet::Int32Statistics>(statistics);
			auto min = statistics->HasMinMax() ? convertedStats->min() : std::numeric_limits<int32_t>::min();
			auto max = statistics->HasMinMax() ? convertedStats->max() : std::numeric_limits<int32_t>::max();
			minmax_metadata_table[col_index].back() = min;
			minmax_metadata_table[col_index + 1].back() = max;

			break;
		}
		case parquet::Type::type::INT64: {
			auto convertedStats = std::static_pointer_cast<parquet::Int64Statistics>(statistics);
			auto min = statistics->HasMinMax() ? convertedStats->min() : std::numeric_limits<int64_t>::min();
			auto max = statistics->HasMinMax() ? convertedStats->max() : std::numeric_limits<int64_t>::max();
			minmax_metadata_table[col_index].back() = min;
			minmax_metadata_table[col_index + 1].back() = max;
			break;
		}
		case parquet::Type::type::FLOAT: {
			auto convertedStats = std::static_pointer_cast<parquet::FloatStatistics>(statistics);
			float min = statistics->HasMinMax() ? convertedStats->min() : std::numeric_limits<float>::min();
			float max = statistics->HasMinMax() ? convertedStats->max() : std::numeric_limits<float>::max();
			// here we want to reinterpret cast minmax_metadata_table to be floats so that we can just use this same vector as if they were floats
			size_t current_row_index = minmax_metadata_table[col_index].size() - 1;
			float* casted_metadata_min = reinterpret_cast<float*>(&(minmax_metadata_table[col_index][0]));
			float* casted_metadata_max = reinterpret_cast<float*>(&(minmax_metadata_table[col_index + 1][0]));
			casted_metadata_min[current_row_index] = min;
			casted_metadata_max[current_row_index] = max;
			break;
		}
		case parquet::Type::type::DOUBLE: {
			auto convertedStats = std::static_pointer_cast<parquet::DoubleStatistics>(statistics);
			double min = statistics->HasMinMax() ? convertedStats->min() : std::numeric_limits<double>::min();
			double max = statistics->HasMinMax() ? convertedStats->max() : std::numeric_limits<double>::max();
			// here we want to reinterpret cast minmax_metadata_table to be double so that we can just use this same vector as if they were double
			size_t current_row_index = minmax_metadata_table[col_index].size() - 1;
			double* casted_metadata_min = reinterpret_cast<double*>(&(minmax_metadata_table[col_index][0]));
			double* casted_metadata_max = reinterpret_cast<double*>(&(minmax_metadata_table[col_index + 1][0]));
			casted_metadata_min[current_row_index] = min;
			casted_metadata_max[current_row_index] = max;
			break;
		}
		case parquet::Type::type::BYTE_ARRAY:
		case parquet::Type::type::FIXED_LEN_BYTE_ARRAY: {
			auto convertedStats =
				std::static_pointer_cast<parquet::FLBAStatistics>(statistics);
			// No min max for String columns
			// minmax_metadata_table[col_index].push_back(-1);
			// minmax_metadata_table[col_index + 1].push_back(-1);
			break;
		}
		case parquet::Type::type::INT96: {
			// "Dont know how to handle INT96 min max"
			// Convert Spark INT96 timestamp to GDF_DATE64
			// return std::make_tuple(GDF_DATE64, 0, 0);
		}
		default:
			throw std::runtime_error("Invalid gdf_dtype in set_min_max");
			break;
		}
	}	
}

// This function is copied and adapted from cudf
cudf::type_id to_dtype(parquet::Type::type physical, parquet::ConvertedType::type logical) {

	// Logical type used for actual data interpretation; the legacy converted type
	// is superceded by 'logical' type whenever available.
	switch (logical) {
	case parquet::ConvertedType::type::UINT_8:
		return cudf::type_id::UINT8;
	case parquet::ConvertedType::type::INT_8:
		return cudf::type_id::INT8;
	case parquet::ConvertedType::type::UINT_16:
		return cudf::type_id::UINT16;
	case parquet::ConvertedType::type::INT_16:
		return cudf::type_id::INT16;
	case parquet::ConvertedType::type::DATE:
		return cudf::type_id::TIMESTAMP_DAYS;
	case parquet::ConvertedType::type::TIMESTAMP_MILLIS:
		return cudf::type_id::TIMESTAMP_MILLISECONDS;
	case parquet::ConvertedType::type::TIMESTAMP_MICROS:
		return cudf::type_id::TIMESTAMP_MICROSECONDS;
	case parquet::ConvertedType::type::DECIMAL: 
		return cudf::type_id::EMPTY;
	default:
		break;
	}

	// Physical storage type supported by Parquet; controls the on-disk storage
	// format in combination with the encoding type.
	switch (physical) {
	case parquet::Type::type::BOOLEAN:
		return cudf::type_id::BOOL8;
	case parquet::Type::type::INT32:
		return cudf::type_id::INT32;
	case parquet::Type::type::INT64:
		return cudf::type_id::INT64;
	case parquet::Type::type::FLOAT:
		return cudf::type_id::FLOAT32;
	case parquet::Type::type::DOUBLE:
		return cudf::type_id::FLOAT64;
	case parquet::Type::type::BYTE_ARRAY:
	case parquet::Type::type::FIXED_LEN_BYTE_ARRAY:
		// TODO: Check GDF_STRING_CATEGORY
		return cudf::type_id::STRING;
	case parquet::Type::type::INT96:
	default:
		break;
	}

	return cudf::type_id::EMPTY;
}

std::unique_ptr<ral::frame::BlazingTable> get_minmax_metadata(
	std::vector<std::unique_ptr<parquet::ParquetFileReader>> &parquet_readers,
	size_t total_num_row_groups, int metadata_offset) {

	if (parquet_readers.size() == 0){
		return nullptr;
	}

	std::vector<std::string> metadata_names;
	std::vector<cudf::data_type> metadata_dtypes;
	std::vector<size_t> columns_with_metadata;

	// NOTE: we must try to use and load always a parquet reader that row groups > 0
	int valid_parquet_reader = -1;

	for (size_t i = 0; i < parquet_readers.size(); ++i) {
		if (parquet_readers[i]->metadata()->num_row_groups() == 0) {
			continue;
		}

		valid_parquet_reader = i;
		break;
	}

	if (valid_parquet_reader == -1){
		const int ncols = parquet_readers[0]->metadata()->schema()->num_columns();
		std::vector<std::string> col_names;
		col_names.resize(ncols);
		for (int i =0; i < ncols; ++i) {
			col_names[i] = parquet_readers[0]->metadata()->schema()->Column(i)->name();
		}
		return make_dummy_metadata_table_from_col_names(col_names);
	}

	std::shared_ptr<parquet::FileMetaData> file_metadata = parquet_readers[valid_parquet_reader]->metadata();

	int num_row_groups = file_metadata->num_row_groups();
	const parquet::SchemaDescriptor *schema = file_metadata->schema();

	if (num_row_groups > 0) {
		auto row_group_index = 0;
		auto groupReader = parquet_readers[valid_parquet_reader]->RowGroup(row_group_index);
		auto *rowGroupMetadata = groupReader->metadata();
		for (int colIndex = 0; colIndex < file_metadata->num_columns(); colIndex++) {
			const parquet::ColumnDescriptor *column = schema->Column(colIndex);
			auto columnMetaData = rowGroupMetadata->ColumnChunk(colIndex);
			auto physical_type = column->physical_type();
			auto logical_type = column->converted_type();
			cudf::data_type dtype = cudf::data_type (to_dtype(physical_type, logical_type)) ;

			if (columnMetaData->is_stats_set() && dtype.id() != cudf::type_id::STRING && dtype.id() != cudf::type_id::EMPTY) {
				auto statistics = columnMetaData->statistics();
					auto col_name_min = "min_" + std::to_string(colIndex) + "_" + column->name();
					metadata_dtypes.push_back(dtype);
					metadata_names.push_back(col_name_min);

					auto col_name_max = "max_" + std::to_string(colIndex)  + "_" + column->name();
					metadata_dtypes.push_back(dtype);
					metadata_names.push_back(col_name_max);

					columns_with_metadata.push_back(colIndex);
			}
		}

		metadata_dtypes.push_back(cudf::data_type{cudf::type_id::INT32});
		metadata_names.push_back("file_handle_index");
		metadata_dtypes.push_back(cudf::data_type{cudf::type_id::INT32});
		metadata_names.push_back("row_group_index");
	}

	size_t num_metadata_cols = metadata_names.size();

	std::vector<std::vector<std::vector<int64_t>>> minmax_metadata_table_per_file(parquet_readers.size());

	std::vector<BlazingThread> threads(parquet_readers.size());
	std::mutex guard;
	for (size_t file_index = 0; file_index < parquet_readers.size(); file_index++){
		// NOTE: It is really important to mantain the `file_index order` in order to match the same order in HiveMetadata
		threads[file_index] = BlazingThread([&guard, metadata_offset,  &parquet_readers, file_index,
									&minmax_metadata_table_per_file, num_metadata_cols, columns_with_metadata](){

		std::shared_ptr<parquet::FileMetaData> file_metadata = parquet_readers[file_index]->metadata();

		if (file_metadata->num_row_groups() > 0){
			std::vector<std::vector<int64_t>> this_minmax_metadata_table(num_metadata_cols);

			int num_row_groups = file_metadata->num_row_groups();
			const parquet::SchemaDescriptor *schema = file_metadata->schema();

			for (int row_group_index = 0; row_group_index < num_row_groups; row_group_index++) {
				auto groupReader = parquet_readers[file_index]->RowGroup(row_group_index);
				auto *rowGroupMetadata = groupReader->metadata();
				for (size_t col_count = 0; col_count < columns_with_metadata.size(); col_count++) {
					const parquet::ColumnDescriptor *column = schema->Column(columns_with_metadata[col_count]);
					auto columnMetaData = rowGroupMetadata->ColumnChunk(columns_with_metadata[col_count]);
					if (columnMetaData->is_stats_set()) {
						auto statistics = columnMetaData->statistics();
						set_min_max(this_minmax_metadata_table,
									col_count * 2,
									column->physical_type(),
									column->converted_type(),
									statistics);
					}
				}
				this_minmax_metadata_table[this_minmax_metadata_table.size() - 2].push_back(metadata_offset + file_index);
				this_minmax_metadata_table[this_minmax_metadata_table.size() - 1].push_back(row_group_index);
			}

			guard.lock();
			minmax_metadata_table_per_file[file_index] = std::move(this_minmax_metadata_table);
			guard.unlock();
		}
		});
	}
	for (size_t file_index = 0; file_index < parquet_readers.size(); file_index++){
		threads[file_index].join();
	}

	std::vector<std::vector<int64_t>> minmax_metadata_table = minmax_metadata_table_per_file[valid_parquet_reader];
	for (size_t i = valid_parquet_reader + 1; i < 	minmax_metadata_table_per_file.size(); i++) {
		for (size_t j = 0; j < 	minmax_metadata_table_per_file[i].size(); j++) {
			std::copy(minmax_metadata_table_per_file[i][j].begin(), minmax_metadata_table_per_file[i][j].end(), std::back_inserter(minmax_metadata_table[j]));
		}
	}

	std::vector<std::unique_ptr<cudf::column>> minmax_metadata_gdf_table(minmax_metadata_table.size());
	for (size_t index = 0; index < 	minmax_metadata_table.size(); index++) {
		auto vector = minmax_metadata_table[index];
		auto dtype = metadata_dtypes[index];
		auto content =  get_typed_vector_content(dtype.id(), vector);
		minmax_metadata_gdf_table[index] = make_cudf_column_from_vector(dtype, content, total_num_row_groups);
	}

	auto table = std::make_unique<cudf::table>(std::move(minmax_metadata_gdf_table));
	return std::make_unique<ral::frame::BlazingTable>(std::move(table), metadata_names);
}
#endif	// BLAZINGDB_RAL_SRC_IO_DATA_PARSER_METADATA_PARQUET_METADATA_CPP_H_
