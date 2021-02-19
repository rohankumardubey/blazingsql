#include "BatchWindowFunctionProcessing.h"
#include "execution_graph/logic_controllers/BlazingColumn.h"
#include "execution_graph/logic_controllers/CacheData.h"
#include "taskflow/executor.h"
#include "CodeTimer.h"

#include <src/utilities/CommonOperations.h>
#include "parser/expression_utils.hpp"
#include <blazingdb/io/Util/StringUtil.h>

#include <cudf/concatenate.hpp>
#include <cudf/stream_compaction.hpp>
#include "cudf/column/column_view.hpp"
#include <cudf/rolling.hpp>
#include <cudf/filling.hpp>
#include <cudf/partitioning.hpp>
#include <cudf/types.hpp>
#include <cudf/copying.hpp>

namespace ral {
namespace batch {

// BEGIN ComputeWindowKernel

ComputeWindowKernel::ComputeWindowKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : kernel{kernel_id, queryString, context, kernel_type::ComputeWindowKernel} {
    this->query_graph = query_graph;
}

// TODO: support for LAG(), LEAD(), currently looks like Calcite has an issue obtaining the optimized plan
// TODO: Support for RANK() and DENSE_RANK() file an cudf feature/request
// TODO: Support for first_value() and last_value() file an cudf feature/request
std::unique_ptr<CudfColumn> ComputeWindowKernel::compute_column_from_window_function(cudf::table_view input_cudf_view, cudf::column_view input_col_view, std::size_t pos) {
    std::unique_ptr<cudf::aggregation> window_function = ral::operators::makeCudfAggregation(this->aggs_wind_func[pos]);
    std::unique_ptr<CudfColumn> windowed_col;
    std::vector<cudf::column_view> table_to_rolling;

    // want all columns to be partitioned
    for (std::size_t col_i = 0; col_i < this->column_indices_partitioned.size(); ++col_i) {
        table_to_rolling.push_back(input_cudf_view.column(this->column_indices_partitioned[col_i]));
    }

    cudf::table_view table_view_with_single_col(table_to_rolling);

    if (this->expression.find("order by") != std::string::npos) {
        // default ROWS/RANGE statement
        if (this->expression.find("UNBOUNDED PRECEDING and CURRENT ROW") != std::string::npos || this->expression.find("between") == std::string::npos) {
            windowed_col = cudf::grouped_rolling_window(table_view_with_single_col , input_col_view, input_col_view.size(), 0, 1, window_function);
        } else {
            throw std::runtime_error("In Window Function: RANGE or ROWS bound is not currently supported");
        }
        
    } else {
        windowed_col = cudf::grouped_rolling_window(table_view_with_single_col , input_col_view, input_col_view.size(), input_col_view.size(), 1, window_function);
    }

    return std::move(windowed_col);
}

ral::execution::task_result ComputeWindowKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& /*args*/) {

    if (inputs.size() == 0) {
        return {ral::execution::task_status::SUCCESS, std::string(), std::vector< std::unique_ptr<ral::frame::BlazingTable> > ()};
    }

    CodeTimer eventTimer(false);

    std::unique_ptr<ral::frame::BlazingTable> & input = inputs[0];

    try{
        cudf::table_view input_cudf_view = input->view();

        // saving the names of the columns and after we will add one by each new col
        std::vector<std::string> input_names = input->names();
        this->column_indices_wind_func = get_columns_to_apply_window_function(this->expression);
        std::tie(this->column_indices_partitioned, std::ignore) = ral::operators::get_vars_to_partition(this->expression);
        std::vector<std::string> aggs_wind_func_str = get_window_function_agg(this->expression); // return MIN  MAX  COUNT

        // fill all the Kind aggregations
        for (std::size_t col_i = 0; col_i < aggs_wind_func_str.size(); ++col_i) {
            AggregateKind aggr_kind_i = ral::operators::get_aggregation_operation(aggs_wind_func_str[col_i]);
            this->aggs_wind_func.push_back(aggr_kind_i);
        }

        std::vector< std::unique_ptr<CudfColumn> > new_wind_funct_cols;
        for (std::size_t col_i = 0; col_i < aggs_wind_func_str.size(); ++col_i) {
            cudf::column_view input_col_view = input_cudf_view.column(column_indices_wind_func[col_i]);

            // calling main window function
            std::unique_ptr<CudfColumn> windowed_col = compute_column_from_window_function(input_cudf_view, input_col_view, col_i);
            new_wind_funct_cols.push_back(std::move(windowed_col));
            input_names.push_back("");
        }

        // Adding these new columns
        std::unique_ptr<cudf::table> cudf_input = input->releaseCudfTable();
        std::vector< std::unique_ptr<CudfColumn> > output_columns = cudf_input->release();
        for (std::size_t col_i = 0; col_i < new_wind_funct_cols.size(); ++col_i) {
            output_columns.push_back(std::move(new_wind_funct_cols[col_i]));
        }

        std::unique_ptr<cudf::table> cudf_table_window = std::make_unique<cudf::table>(std::move(output_columns));
        std::unique_ptr<ral::frame::BlazingTable> windowed_table = std::make_unique<ral::frame::BlazingTable>(std::move(cudf_table_window), input_names);

        if (windowed_table) {
            cudf::size_type num_rows = windowed_table->num_rows();
            std::size_t num_bytes = windowed_table->sizeInBytes();

        }

        output->addToCache(std::move(windowed_table));
    }catch(rmm::bad_alloc e){
        return {ral::execution::task_status::RETRY, std::string(e.what()), std::move(inputs)};
    }catch(std::exception e){
        return {ral::execution::task_status::FAIL, std::string(e.what()), std::vector< std::unique_ptr<ral::frame::BlazingTable> > ()};
    }

    return {ral::execution::task_status::SUCCESS, std::string(), std::vector< std::unique_ptr<ral::frame::BlazingTable> > ()};
}

kstatus ComputeWindowKernel::run() {
    CodeTimer timer;

    std::unique_ptr<ral::cache::CacheData> cache_data = this->input_cache()->pullCacheData();

    while (cache_data != nullptr ){
        std::vector<std::unique_ptr <ral::cache::CacheData> > inputs;
        inputs.push_back(std::move(cache_data));

        ral::execution::executor::get_instance()->add_task(
                std::move(inputs),
                this->output_cache(),
                this);

        cache_data = this->input_cache()->pullCacheData();
    }

    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty();
    });

    if (logger != nullptr) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="ComputeWindow Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }
    return kstatus::proceed;
}

// END ComputeWindowKernel


// START OverlapAccumulatorKernel

OverlapAccumulatorKernel::OverlapAccumulatorKernel(std::size_t kernel_id, const std::string & queryString,
    std::shared_ptr<Context> context,
    std::shared_ptr<ral::cache::graph> query_graph)
    : distributing_kernel{kernel_id, queryString, context, kernel_type::OverlapAccumulatorKernel} {
    this->query_graph = query_graph;
    this->input_.add_port("batches", "presceding_overlaps", "following_overlaps");

    this->num_batches = 0;
	this->have_all_batches = false;

    input_batches_cache = this->input_.get_cache("batches");
    input_presceding_overlap_cache = this->input_.get_cache("presceding_overlaps");
    input_following_overlap_cache = this->input_.get_cache("following_overlaps");

    // WSM TODO make these and the join cacheMachines, be array_cache
    ral::cache::cache_settings cache_machine_config;
	cache_machine_config.type = ral::cache::CacheType::SIMPLE;
	cache_machine_config.context = context->clone();

    std::string batches_cache_name = std::to_string(this->get_id()) + "_batches";
    this->batches_cache = ral::cache::create_cache_machine(cache_machine_config, batches_cache_name);
    std::string presceding_cache_name = std::to_string(this->get_id()) + "_presceding";
	this->presceding_overlap_cache = ral::cache::create_cache_machine(cache_machine_config, presceding_cache_name);
    std::string following_cache_name = std::to_string(this->get_id()) + "_following";
	this->following_overlap_cache = ral::cache::create_cache_machine(cache_machine_config, following_cache_name);

    auto& self_node = ral::communication::CommunicationData::getInstance().getSelfNode();
	self_node_index = context->getNodeIndex(self_node);
    
}

const std::string UNKNOWN_OVERLAP_STATUS="UNKNOWN";
const std::string REQUESTED_OVERLAP_STATUS="REQUESTED";
const std::string INCOMPLETE_OVERLAP_STATUS="INCOMPLETE";
const std::string PROCESSING_OVERLAP_STATUS="PROCESSING"; // WSM TODO, do we need this?
const std::string DONE_OVERLAP_STATUS="DONE";

const std::string TASK_ARG_OP_TYPE="operation_type";
const std::string TASK_ARG_OVERLAP_TYPE="overlap_type";
const std::string TASK_ARG_OVERLAP_SIZE="overlap_size";
const std::string TASK_ARG_SOURCE_BATCH_INDEX="source_batch_index";
const std::string TASK_ARG_TARGET_BATCH_INDEX="target_batch_index";
const std::string TASK_ARG_TARGET_NODE_INDEX="target_node_index";

const std::string OVERLAP_TASK_TYPE="get_overlap";
const std::string PRESCEDING_OVERLAP_TYPE="presceding";
const std::string FOLLOWING_OVERLAP_TYPE="following";
const std::string NODE_COMPLETED_REQUEST="node_completed";
const std::string PRESCEDING_REQUEST="presceding_request";
const std::string FOLLOWING_REQUEST="following_request";
const std::string PRESCEDING_FULFILLMENT="presceding_fulfillment";
const std::string FOLLOWING_FULFILLMENT="following_fulfillment";


void OverlapAccumulatorKernel::set_overlap_status(bool presceding, int index, std::string status){
    std::lock_guard<std::mutex> lock(kernel_mutex);
    if (presceding){
        presceding_overlap_statuses[index] = status;
    } else {
        following_overlap_status[index] = status;
    }

    if (have_all_batches){
        bool all_done = true;
        for (int i = 0; i < num_batches; i++){
            if ((presceding && presceding_overlap_statuses[i] !=  DONE_OVERLAP_STATUS) ||
                    (!presceding && following_overlap_status[i] !=  DONE_OVERLAP_STATUS)){
                all_done = false;
            }
        }
        if (all_done){ // if all done lets send notification
            ral::cache::MetadataDictionary extra_metadata;
            extra_metadata.add_value(ral::cache::OVERLAP_MESSAGE_TYPE, NODE_COMPLETED_REQUEST);
            
            std::vector<std::string> target_ids;
            if (presceding && this->self_node_index > 0){
                target_ids.push_back(std::to_string(this->self_node_index - 1));
            }
            if (!presceding && this->self_node_index + 1 < context->getTotalNodes()){
                target_ids.push_back(std::to_string(this->self_node_index + 1));
            }
            if (target_ids.size() > 0){
                send_message(nullptr,
                    false, //specific_cache
                    "", //cache_id
                    target_ids, //target_ids
                    "", //message_id_prefix
                    true, //always_add
                    false, //wait_for
                    0, //message_tracker_idx
                    extra_metadata);
            }
            kernel_cv.notify_all(); // WSM DO WE NEED THIS?            
        }        
    }      
}

std::string OverlapAccumulatorKernel::get_overlap_status(bool presceding, int index){
    std::lock_guard<std::mutex> lock(kernel_mutex);
    if (presceding){
        return presceding_overlap_statuses[index];
    } else {
        return following_overlap_status[index];
    }
}

void OverlapAccumulatorKernel::combine_overlaps(bool presceding, int target_batch_index, std::unique_ptr<ral::frame::BlazingTable> new_overlap, std::string overlap_status) {
    
    // WSM TODO should make a function that can create a cache data and automatically cache it if the resouce consumption demands it
    std::unique_ptr<ral::cache::CacheData> new_overlap_cache_data = std::make_unique<ral::cache::GPUCacheData>(new_overlap);
    return combine_overlaps(presceding, target_batch_index, std::move(new_overlap_cache_data), overlap_status);
}

void OverlapAccumulatorKernel::combine_overlaps(bool presceding, int target_batch_index, std::unique_ptr<ral::cache::CacheData> new_overlap_cache_data, std::string overlap_status) {
    
    std::vector<std::unique_ptr<ral::cache::CacheData>> overlap_parts;
    std::unique_ptr<ral::cache::CacheData> existing_overlap;
    if (presceding){
        existing_overlap = presceding_overlap_cache->get_or_wait_CacheData(target_batch_index);
    } else { 
        existing_overlap = following_overlap_cache->get_or_wait_CacheData(target_batch_index);
    }
    
    if (existing_overlap->get_type() == ral::cache::CacheDataType::CONCATENATING){
        ral::cache::ConcatCacheData * concat_cache_ptr = static_cast<ral::cache::ConcatCacheData *> (existing_overlap.get());
        overlap_parts = concat_cache_ptr->releaseCacheDatas();       
    } else {
        overlap_parts.push_back(existing_overlap);
    }
    if (presceding){
        overlap_parts.insert(overlap_parts.begin(), 1, new_overlap_cache_data);
    } else {
        overlap_parts.push_back(new_overlap_cache_data);
    }
    
    std::unique_ptr<ral::cache::ConcatCacheData> new_cache_data = std::make_unique<ral::cache::ConcatCacheData>(std::move(overlap_parts), this->col_names, this->schema);
    if (presceding){
        presceding_overlap_cache->put(target_batch_index, std::move(new_cache_data));        
    } else { 
        following_overlap_cache->put(target_batch_index, std::move(new_cache_data));        
    }
    set_overlap_status(presceding, target_batch_index, overlap_status);    
}


ral::execution::task_result OverlapAccumulatorKernel::do_process(std::vector< std::unique_ptr<ral::frame::BlazingTable> > inputs,
    std::shared_ptr<ral::cache::CacheMachine> output,
    cudaStream_t /*stream*/, const std::map<std::string, std::string>& args) {

    try {
        std::string operation_type = args.at(TASK_ARG_OP_TYPE);
        std::string overlap_type = args.at(TASK_ARG_OVERLAP_TYPE);
        size_t overlap_size = std::stoll(args.at(TASK_ARG_OVERLAP_SIZE));
        int source_batch_index = std::stoi(args.at(TASK_ARG_SOURCE_BATCH_INDEX));
        int target_batch_index = std::stoi(args.at(TASK_ARG_TARGET_BATCH_INDEX));
        int target_node_index = std::stoi(args.at(TASK_ARG_TARGET_NODE_INDEX));
        std::string overlap_status = args.at(ral::cache::OVERLAP_STATUS);

        bool presceding = overlap_type == PRESCEDING_OVERLAP_TYPE;

        if (operation_type == OVERLAP_TASK_TYPE){

            std::vector< std::unique_ptr<ral::frame::BlazingTable> > scope_holder;
            std::vector<ral::frame::BlazingTableView> tables_to_concat;
            size_t rows_remaining = overlap_size;

            if (presceding) {
                
                for (int i = inputs.size() -1; i >= 0; i--){
                    size_t cur_table_size = inputs[i]->num_rows();
                    if (cur_table_size > rows_remaining){
                        bool front = false;
                        auto limited = ral::utilities::getLimitedRows(inputs[i]->toBlazingTableView(), rows_remaining, front);
                        tables_to_concat.insert(tables_to_concat.begin(), 1, limited->toBlazingTableView());
                        scope_holder.push_back(std::move(limited));
                        rows_remaining = 0;
                        break;
                    } else {
                        rows_remaining -= cur_table_size;
                        tables_to_concat.insert(tables_to_concat.begin(), 1, inputs[i]->toBlazingTableView());
                    }
                }

            } else { // if (overlap_type == FOLLOWING_OVERLAP_TYPE) {

                for (int i = 0; i < inputs.size(); i++){
                    size_t cur_table_size = inputs[i]->num_rows();
                    if (cur_table_size > rows_remaining){
                        bool front = true;
                        auto limited = ral::utilities::getLimitedRows(inputs[i]->toBlazingTableView(), rows_remaining, front);
                        tables_to_concat.push_back(limited->toBlazingTableView());
                        scope_holder.push_back(std::move(limited));
                        rows_remaining = 0;
                        break;
                    } else {
                        rows_remaining -= cur_table_size;
                        tables_to_concat.push_back(inputs[i]->toBlazingTableView());
                    }
                }
            }
            
            
            std::unique_ptr<ral::frame::BlazingTable> output_table;
            if (tables_to_concat.size() == 1 && scope_holder.size() == 1) {
                output_table = std::move(scope_holder[0]);                
            } else {
                output_table = ral::utilities::concatTables(tables_to_concat);
            }

            if (this->self_node_index == target_node_index) {
                combine_overlaps(presceding, target_batch_index, std::move(output_table), overlap_status);

            } else {
                 //send to node
                ral::cache::MetadataDictionary extra_metadata;
                extra_metadata.add_value(ral::cache::OVERLAP_MESSAGE_TYPE, presceding ? PRESCEDING_FULFILLMENT : FOLLOWING_FULFILLMENT);
                extra_metadata.add_value(ral::cache::OVERLAP_SOURCE_NODE_INDEX, std::to_string(this->self_node_index));
                extra_metadata.add_value(ral::cache::OVERLAP_TARGET_NODE_INDEX, std::to_string(target_node_index));
                extra_metadata.add_value(ral::cache::OVERLAP_TARGET_BATCH_INDEX, std::to_string(target_batch_index));
                extra_metadata.add_value(ral::cache::OVERLAP_STATUS, overlap_status);
                
                std::vector<std::string> target_ids = {std::to_string(target_node_index)};
                send_message(std::move(output_table),
                    false, //specific_cache
                    "", //cache_id
                    target_ids, //target_ids
                    "", //message_id_prefix
                    true, //always_add
                    false, //wait_for
                    0, //message_tracker_idx
                    extra_metadata);
            }

            // now lets put the input data back where it belongs
            for (int i = 0; i < inputs.size(); i++){
                if (source_batch_index == -1){
                    presceding_overlap_cache->put(0, std::move(inputs[i]));
                } else if (source_batch_index == num_batches) {
                    following_overlap_cache->put(num_batches-1, std::move(inputs[i]));
                } else {
                    batches_cache->put(source_batch_index, std::move(inputs[i]));
                }
                source_batch_index++;
            }

        }
    }catch(rmm::bad_alloc e){
        return {ral::execution::task_status::RETRY, std::string(e.what()), std::move(inputs)};
    }catch(std::exception e){
        return {ral::execution::task_status::FAIL, std::string(e.what()), std::vector< std::unique_ptr<ral::frame::BlazingTable> > ()};
    }

    return {ral::execution::task_status::SUCCESS, std::string(), std::vector< std::unique_ptr<ral::frame::BlazingTable> > ()};
}

void OverlapAccumulatorKernel::request_receiver(){

    int total_nodes = context->getTotalNodes();
    int messages_expected = 4;
    if (self_node_index == 0){
        messages_expected = 2;
    } else if (self_node_index == total_nodes - 1) {
        messages_expected = 2;
    } 
    
    int messages_received = 0;
    while(messages_received < messages_expected){
        std::string message_id = std::to_string(this->context->getContextToken()) + "_" + std::to_string(this->get_id());
        auto message_cache_data = this->query_graph->get_input_message_cache()->pullCacheData(message_id);
        auto metadata = message_cache_data->getMetadata();
        messages_received++;
        if (metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == PRESCEDING_REQUEST
            || metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == FOLLOWING_REQUEST){

            size_t overlap_size = std::stoll(metadata.get_value(ral::cache::OVERLAP_SIZE));
            int target_batch_index = std::stoi(metadata.get_value(ral::cache::OVERLAP_TARGET_NODE_INDEX));
            int target_node_index = std::stoi(metadata.get_value(ral::cache::OVERLAP_TARGET_BATCH_INDEX));
            int source_batch_index = metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == PRESCEDING_REQUEST ? num_batches : 0;

            prepare_overlap_task(metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == PRESCEDING_REQUEST, 
                source_batch_index, target_node_index, target_batch_index, overlap_size);
            
        } else if (metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == PRESCEDING_FULFILLMENT
                        || metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == FOLLOWING_FULFILLMENT){

            int source_node_index = std::stoi(metadata.get_value(ral::cache::OVERLAP_SOURCE_NODE_INDEX));
            int target_node_index = std::stoi(metadata.get_value(ral::cache::OVERLAP_TARGET_NODE_INDEX));
            int target_batch_index = std::stoi(metadata.get_value(ral::cache::OVERLAP_TARGET_BATCH_INDEX));
            bool presceding = metadata.get_value(ral::cache::OVERLAP_MESSAGE_TYPE) == PRESCEDING_FULFILLMENT;
            std::string overlap_status = metadata.get_value(ral::cache::OVERLAP_STATUS);

            if (target_node_index != self_node_index){
                // WSM TODO "ERROR: FULFILLMENT message arrived at the wrong destination"
            } 
            combine_overlaps(presceding, target_batch_index, std::move(message_cache_data), overlap_status);
                        
        } else {
            // TODO throw ERROR unknown request type in window function
        }
    }
}

void OverlapAccumulatorKernel::prepare_overlap_task(bool presceding, int source_batch_index, int target_node_index, int target_batch_index, size_t overlap_size){
     
    std::vector<std::unique_ptr<ral::cache::CacheData>> cache_datas_for_task;
    size_t overlap_rows_needed = overlap_size;
    int starting_index_of_datas_for_task = source_batch_index;
    while(overlap_rows_needed > 0){
        // Lets first try to fulfill the overlap needed from this node
        if (source_batch_index >= 0 && source_batch_index < this->num_batches){  // num_batches should be finalized for when its used here
                        
            std::unique_ptr<ral::frame::CacheData> batch = batches_cache->get_or_wait_CacheData(source_batch_index);
            overlap_rows_needed = batch->num_rows() > overlap_rows_needed ? 0 : overlap_rows_needed - batch->num_rows();
            if (presceding){
                starting_index_of_datas_for_task = source_batch_index;
                source_batch_index--;
                cache_datas_for_task.insert(cache_datas_for_task.begin(), 1, std::move(batch));
            } else {
                source_batch_index++;
                cache_datas_for_task.push_back(std::move(batch));
            }
        } else {
            // if we did not get enough from the regular batches, then lets try to get the data from the last overlap
            if (presceding){
                // the 0th index of the presceding node will come from the neighbor. Its assumed that its complete.
                // and if its not complete its because there is not enough data to fill the window
                std::unique_ptr<ral::frame::CacheData> batch = presceding_overlap_cache->get_or_wait_CacheData(0);
                overlap_rows_needed = 0;
                cache_datas_for_task.insert(cache_datas_for_task.begin(), 1, std::move(batch));
                starting_index_of_datas_for_task = -1;                                
            } else {
                // the last index of the following node will come from the neighbor. Its assumed that its complete.
                // and if its not complete its because there is not enough data to fill the window
                std::unique_ptr<ral::frame::CacheData> batch = following_overlap_cache->get_or_wait_CacheData(this->num_batches - 1);
                overlap_rows_needed = 0;
                cache_datas_for_task.push_back(std::move(batch));                 
            }             
        }
    }
    std::string overlap_status = overlap_rows_needed > 0 ? INCOMPLETE_OVERLAP_STATUS : DONE_OVERLAP_STATUS;
    if (cache_datas_for_task.size() > 0){ // we have data, so lets make a task
        
        std::map<std::string, std::string> task_args;
        task_args[TASK_ARG_OP_TYPE] = OVERLAP_TASK_TYPE;
        task_args[TASK_ARG_OVERLAP_TYPE] = presceding ? PRESCEDING_OVERLAP_TYPE : FOLLOWING_OVERLAP_TYPE;
        task_args[TASK_ARG_OVERLAP_SIZE] = std::to_string(overlap_size);
        task_args[TASK_ARG_TARGET_BATCH_INDEX] = std::to_string(target_batch_index);
        task_args[TASK_ARG_TARGET_NODE_INDEX] = std::to_string(target_node_index);
        task_args[TASK_ARG_SOURCE_BATCH_INDEX] = std::to_string(starting_index_of_datas_for_task);
        task_args[OVERLAP_STATUS] = overlap_status;
        ral::execution::executor::get_instance()->add_task(
            std::move(cache_datas_for_task),
            presceding ? presceding_overlap_cache : following_overlap_cache,
            this,
            task_args);
    }    
}

void OverlapAccumulatorKernel::send_request(bool presceding, int source_node_index, int target_node_index, int target_batch_index, size_t overlap_size){
    ral::cache::MetadataDictionary extra_metadata;
    extra_metadata.add_value(ral::cache::OVERLAP_MESSAGE_TYPE, presceding ? PRESCEDING_REQUEST : FOLLOWING_REQUEST);
    extra_metadata.add_value(ral::cache::OVERLAP_SIZE, std::to_string(overlap_size));
    extra_metadata.add_value(ral::cache::OVERLAP_TARGET_NODE_INDEX, std::to_string(target_node_index));
    extra_metadata.add_value(ral::cache::OVERLAP_TARGET_BATCH_INDEX, std::to_string(target_batch_index));
    extra_metadata.add_value(ral::cache::OVERLAP_SOURCE_NODE_INDEX, std::to_string(source_node_index));

    std::vector<std::string> target_ids = {std::to_string(source_node_index)};
    send_message(nullptr,
        false, //specific_cache
        "", //cache_id
        target_ids, //target_ids
        "", //message_id_prefix
        true, //always_add
        false, //wait_for
        0, //message_tracker_idx
        extra_metadata);
}

kstatus OverlapAccumulatorKernel::run() {

    bool all_done = false;
    bool neighbors_notified_of_complete = false;
    int total_nodes = context->getTotalNodes();
    
    int cur_batch_ind = 0;
    while (!have_all_batches){

        auto batch = input_batches_cache->pullCacheData();
        if (batch != nullptr) {
            if (col_names.size() == 0){
                // we want to have this in case we need to make an empty table
                this->col_names = batch->names();
                this->schema = batch->get_schema();
            }
            batches_cache->put(cur_batch_ind, std::move(batch));
            num_batches = cur_batch_ind + 1;
            cur_batch_ind++;            
        } else {
            have_all_batches = true;
        }
    }
    presceding_overlap_statuses.resize(num_batches, UNKNOWN_OVERLAP_STATUS);
    following_overlap_status.resize(num_batches, UNKNOWN_OVERLAP_STATUS);
    
    // lets send the requests for the first presceding overlap and last following overlap of this node
    send_request(true, self_node_index - 1, self_node_index, 0, this->presceding_overlap_amount);
    send_request(false, self_node_index + 1, self_node_index, num_batches-1, this->following_overlap_amount);

    BlazingThread receiver_thread = BlazingThread(&request_receiver);
    
    for (int cur_batch_ind = 0; cur_batch_ind < num_batches; cur_batch_ind++){      
        if (cur_batch_ind == 0){
            if (self_node_index == 0){ // first overlap of first node, so make it empty
                std::unique_ptr<ral::frame::BlazingTable> empty_table = ral::utilities::create_empty_table(this->col_names, this->schema);
                presceding_overlap_cache->put(cur_batch_ind, std::move(empty_table));
            } // else, the first presceding overlap we already sent a request to our neighbor               
            
        } else {
            auto overlap_cache_data = input_presceding_overlap_cache->pullCacheData();
            if (overlap_cache_data != nullptr){
                auto metadata = overlap_cache_data->getMetadata();
                size_t cur_overlap_rows = overlap_cache_data->num_rows();
                if (!metadata.has_value(ral::cache::OVERLAP_STATUS)){
                    // WSM TODO "ERROR: Overlap Data did not have OVERLAP_STATUS"
                }
                set_overlap_status(true, cur_batch_ind, metadata.get_value(ral::cache::OVERLAP_STATUS));
                presceding_overlap_cache->put(cur_batch_ind, std::move(overlap_cache_data));
                
                if (metadata.get_value(ral::cache::OVERLAP_STATUS == INCOMPLETE_OVERLAP_STATUS){
                    prepare_overlap_task(true, cur_batch_ind - 1, this->self_node_index, cur_batch_ind, 
                                    this->presceding_overlap_amount - cur_overlap_rows);                    
                }
            } else {
                // WSM TODO error
            }
        }
    
        if (cur_batch_ind < num_batches - 1){
            auto overlap_cache_data = input_following_overlap_cache->pullCacheData();
            if (overlap_cache_data != nullptr){
                auto metadata = overlap_cache_data->getMetadata();
                size_t cur_overlap_rows = overlap_cache_data->num_rows();
                if (!metadata.has_value(ral::cache::OVERLAP_STATUS)){
                    // WSM TODO "ERROR: Overlap Data did not have OVERLAP_STATUS"
                }
                set_overlap_status(false, cur_batch_ind, metadata.get_value(ral::cache::OVERLAP_STATUS));
                following_overlap_cache->put(cur_batch_ind, std::move(overlap_cache_data));
                
                if (metadata.get_value(ral::cache::OVERLAP_STATUS == INCOMPLETE_OVERLAP_STATUS){
                    prepare_overlap_task(false, cur_batch_ind + 1, this->self_node_index, cur_batch_ind, 
                                    this->following_overlap_amount - cur_overlap_rows);                    
                }
            } else {
                // WSM TODO error
            }
        } else {
            if (self_node_index == total_nodes - 1){ // last overlap of last node, so make it empty
                std::unique_ptr<ral::frame::BlazingTable> empty_table = ral::utilities::create_empty_table(this->col_names, this->schema);
                following_overlap_cache->put(cur_batch_ind, std::move(empty_table));
            } // else, the last following overlap we already sent a request to our neighbor               
        }
    }
     
    // lets wait until the receiver thread is done. 
    // When its done, it means we have received overlap requests and have make tasks for them, and
    // it also means we have received the fulfillments overlap requests we sent out
    receiver_thread.join();

    // lets wait to make sure that all tasks are done
    std::unique_lock<std::mutex> lock(kernel_mutex);
    kernel_cv.wait(lock,[this]{
        return this->tasks.empty() || ral::execution::executor::get_instance()->has_exception();
    });
    if(auto ep = ral::execution::executor::get_instance()->last_exception()){
        std::rethrow_exception(ep);
    }

    // Now that we are all done, lets concatenate the overlaps with the data and push to the output
    for (size_t batch_ind = 0; batch_ind < num_batches; batch_ind++){
        std::vector<std::unique_ptr<ral::frame::CacheData>> batch_with_overlaps;
        batch_with_overlaps.push_back(presceding_overlap_cache->get_or_wait_CacheData(batch_ind));
        batch_with_overlaps.push_back(batches_cache->get_or_wait_CacheData(source_batch_index));
        batch_with_overlaps.push_back(following_overlap_cache->get_or_wait_CacheData(batch_ind));

        std::unique_ptr<ral::cache::ConcatCacheData> new_cache_data = std::make_unique<ral::cache::ConcatCacheData>(std::move(batch_with_overlaps), col_names, schema);
        this->add_to_output_cache(new_cache_data);
    }

    if(logger) {
        logger->debug("{query_id}|{step}|{substep}|{info}|{duration}|kernel_id|{kernel_id}||",
                    "query_id"_a=context->getContextToken(),
                    "step"_a=context->getQueryStep(),
                    "substep"_a=context->getQuerySubstep(),
                    "info"_a="OverlapAccumulatorKernel Kernel Completed",
                    "duration"_a=timer.elapsed_time(),
                    "kernel_id"_a=this->get_id());
    }

    // these are intra kernel caches. We want to make sure they are empty before we finish.
    this->batches_cache->clear();
    this->presceding_overlap_cache->clear();
    this->following_overlap_cache->clear();

    return kstatus::proceed;
        
    
}

/* Ideas for when we want to implement RANGE window frame instead of ROWS window frame:
The previous kernel if there is RANGE needs to add metadata to every batch and overlap about the value of the first and last element
Then when preparing the overlapping tasks we can see how many batches we need to fulfill the window, just by looking at the metadata about the 
first and last elements.
*/

/* A few words on the efficiency of this algorithm:

This logic that has been implemented has the downside of waiting until all batches are available so that we know the number of batches.
We also cant push results to the next phase until we know we have fulfilled our obligations to the neighboring nodes.
This was done to dramatically simplify the logic. Additionally its not as bad of a performance penalty because the previous kernel which does an 
order by, also needs to wait until all batches are available before it can do its merge.
In the future, when we can have CacheData's shared between nodes, then we can revisit this logic to make it more efficient.
*/

// END OverlapAccumulatorKernel

} // namespace batch
} // namespace ral
