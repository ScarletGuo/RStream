/*
 * mining_phase.hpp
 *
 *  Created on: Jun 15, 2017
 *      Author: kai
 */

#ifndef CORE_MINING_PHASE_HPP_
#define CORE_MINING_PHASE_HPP_

#include "scatter.hpp"

namespace RStream {
	/*
	 *  Graph mining support. Join on all keys for each vertex tuple.
	 *  Each element in the tuple contains 8 bytes, first 4 bytes is vertex id,
	 *  second 4 bytes contains edge label(1byte) + vertex label(1byte) + history info(1byte).
	 *  History info is used to record subgraph structure.
	 *
	 *
	 *  [ ] [ ] [ ] [ ] || [ ] [ ] [ ] [ ]
	 *    vertex id        idx  el  vl info
	 *     4 bytes          1   1   1    1
	 *
	 * */
	struct Element_In_Tuple {
		VertexId vertex_id;
		BYTE key_index;
		BYTE edge_label;
		BYTE vertex_label;
		BYTE history_info;

		Element_In_Tuple(int _vertex_id, BYTE _edge_label, BYTE _vertex_label) :
			vertex_id(_vertex_id), edge_label(_edge_label), vertex_label(_vertex_label), key_index(0), history_info(0) {

		}
	};

	// One tuple contains multiple elements. "size" is the num of elements in one tuple
	struct Update_Tuple {
		std::vector<Element_In_Tuple> elements;
	};

	class MPhase {
		const Engine & context;
		std::atomic<int> atomic_num_producers;
		std::atomic<int> atomic_partition_id;
		std::atomic<int> atomic_partition_number;

	public:
		// num of bytes for in_update_tuple
		static int sizeof_in_tuple;
		// num of bytes for out_update_tuple
		static int sizeof_out_tuple;

		MPhase(Engine & e) : context(e) {
			atomic_num_producers = 0;
			atomic_partition_id = 0;
			atomic_partition_number = context.num_partitions;
		}

		~MPhase() {}

		// TODO:
		Update_Stream init() {}

		/* join update stream with edge stream, shuffle on all keys
		 * @param in_update_stream -input file for update stream
		 * @param out_update_stream -output file for update stream
		 * */
		Update_Stream join_all_keys(Update_Stream in_update_stream) {

			// each element in the tuple is 2 ints (16 bytes)
			sizeof_out_tuple = sizeof_in_tuple + 16;
			Update_Stream update_c = Engine::update_count++;

			concurrent_queue<int> * task_queue = new concurrent_queue<int>(context.num_partitions);

			// push task into concurrent queue
			for(int partition_id = 0; partition_id < context.num_partitions; partition_id++) {
				task_queue->push(partition_id);
			}

			// allocate global buffers for shuffling
			global_buffer_for_mining ** buffers_for_shuffle = buffer_manager_for_mining::get_global_buffers_for_mining(context.num_partitions, sizeof_out_tuple);

			// exec threads will produce updates and push into shuffle buffers
			std::vector<std::thread> exec_threads;
			for(int i = 0; i < context.num_exec_threads; i++)
				exec_threads.push_back( std::thread([=] { this->join_all_keys_producer(in_update_stream, buffers_for_shuffle, task_queue); } ));

			// write threads will flush shuffle buffer to update out stream file as long as it's full
			std::vector<std::thread> write_threads;
			for(int i = 0; i < context.num_write_threads; i++)
				write_threads.push_back(std::thread(&MPhase::join_all_keys_consumer, this, update_c, buffers_for_shuffle));

			// join all threads
			for(auto & t : exec_threads)
				t.join();

			for(auto &t : write_threads)
				t.join();

			delete[] buffers_for_shuffle;
			delete task_queue;

			sizeof_in_tuple = sizeof_out_tuple;

			return update_c;
		}

	private:
		// each exec thread generates a join producer
		void join_all_keys_producer(Update_Stream in_update_stream, global_buffer_for_mining ** buffers_for_shuffle, concurrent_queue<int> * task_queue) {
			atomic_num_producers++;
			int partition_id = -1;

			// pop from queue
			while(task_queue->test_pop_atomic(partition_id)){
				std::cout << partition_id << std::endl;

				int fd_update = open((context.filename + "." + std::to_string(partition_id) + ".update_stream_" + std::to_string(in_update_stream)).c_str(), O_RDONLY);
				int fd_edge = open((context.filename + "." + std::to_string(partition_id)).c_str(), O_RDONLY);
				assert(fd_update > 0 && fd_edge > 0 );

				// get file size
				long update_file_size = io_manager::get_filesize(fd_update);
				long edge_file_size = io_manager::get_filesize(fd_edge);

				print_thread_info_locked("as a producer dealing with partition " + std::to_string(partition_id) + "\n");

				// streaming updates
				char * update_local_buf = (char *)memalign(PAGE_SIZE, IO_SIZE);
				int streaming_counter = update_file_size / IO_SIZE + 1;

				// edges are fully loaded into memory
				char * edge_local_buf = new char[edge_file_size];
				io_manager::read_from_file(fd_edge, edge_local_buf, edge_file_size, 0);

				// build edge hashmap
				const int n_vertices = context.vertex_intervals[partition_id].second - context.vertex_intervals[partition_id].first + 1;
				int vertex_start = context.vertex_intervals[partition_id].first;
				assert(n_vertices > 0 && vertex_start >= 0);

//				std::array<std::vector<VertexId>, num_vertices> edge_hashmap;
				std::vector<Element_In_Tuple> edge_hashmap[n_vertices];
				build_edge_hashmap(edge_local_buf, edge_hashmap, edge_file_size, vertex_start);

				long valid_io_size = 0;
				long offset = 0;

				// for all streaming updates
				for(int counter = 0; counter < streaming_counter; counter++) {
					// last streaming
					if(counter == streaming_counter - 1)
						// TODO: potential overflow?
						valid_io_size = update_file_size - IO_SIZE * (streaming_counter - 1);
					else
						valid_io_size = IO_SIZE;

					assert(valid_io_size % sizeof_in_tuple == 0);

					io_manager::read_from_file(fd_update, update_local_buf, valid_io_size, offset);
					offset += valid_io_size;

					// streaming updates in, do hash join
					for(long pos = 0; pos < valid_io_size; pos += sizeof_in_tuple) {
						// get an in_update_tuple
						std::vector<Element_In_Tuple> & in_update_tuple = get_an_in_update(update_local_buf + pos);

						// get key index
						BYTE key_index = get_key_index(in_update_tuple);
						assert(key_index >= 0 && key_index < in_update_tuple.size());

						// get vertex_id as the key to index edge hashmap
						VertexId key = in_update_tuple.at(key_index).vertex_id;

						for(Element_In_Tuple element : edge_hashmap[key - vertex_start]) {
							// generate a new out update tuple
							std::vector<Element_In_Tuple> & out_update_tuple = gen_an_out_update(in_update_tuple, element, key_index);

							shuffle_on_all_keys(out_update_tuple, buffers_for_shuffle);

						}

					}
				}

				delete[] update_local_buf;
				delete[] edge_local_buf;

				close(fd_update);
				close(fd_edge);
			}

			atomic_num_producers--;
		}

		// each writer thread generates a join_consumer
		void join_all_keys_consumer(Update_Stream out_update_stream, global_buffer_for_mining ** buffers_for_shuffle) {
			while(atomic_num_producers != 0) {
				int i = (atomic_partition_id++) % context.num_partitions ;

				const char * file_name = (context.filename + "." + std::to_string(i) + ".update_stream_" + std::to_string(out_update_stream)).c_str();
				global_buffer_for_mining* g_buf = buffer_manager_for_mining::get_global_buffer_for_mining(buffers_for_shuffle, context.num_partitions, i);
				g_buf->flush(file_name, i);
			}

			//the last run - deal with all remaining content in buffers
			while(true){
				int i = --atomic_partition_number;

				if(i >= 0){

					const char * file_name = (context.filename + "." + std::to_string(i) + ".update_stream_" + std::to_string(out_update_stream)).c_str();
					global_buffer_for_mining* g_buf = buffer_manager_for_mining::get_global_buffer_for_mining(buffers_for_shuffle, context.num_partitions, i);
					g_buf->flush_end(file_name, i);

					delete g_buf;
				}
				else{
					break;
				}
			}
		}

		void shuffle_on_all_keys(std::vector<Element_In_Tuple> & out_update_tuple, global_buffer_for_mining ** buffers_for_shuffle) {
			char* out_update = nullptr;

			// shuffle on all other keys
			for(int i = 0; i < out_update_tuple.size(); i++) {

				out_update = reinterpret_cast<char*>(out_update_tuple.data());

				set_key_index(out_update_tuple, i);
				VertexId key = out_update_tuple.at(i).vertex_id;
				int index = get_global_buffer_index(key);
				global_buffer_for_mining* global_buf = buffer_manager_for_mining::get_global_buffer_for_mining(buffers_for_shuffle, context.num_partitions, index);
				global_buf->insert(out_update);

			}

		}

		std::vector<Element_In_Tuple> & gen_an_out_update(std::vector<Element_In_Tuple> & in_update_tuple, Element_In_Tuple & element, BYTE history) {
			std::vector<Element_In_Tuple> out_update_tuple(in_update_tuple);
			element.history_info = history;
			out_update_tuple.push_back(element);

			return out_update_tuple;
		}


		std::vector<Element_In_Tuple> & get_an_in_update(char * update_local_buf) {
			std::vector<Element_In_Tuple> tuple;

			for(int index = 0; index < sizeof_in_tuple; index += sizeof(Element_In_Tuple)) {
				Element_In_Tuple & element = *(Element_In_Tuple*)(update_local_buf + index);
				tuple.push_back(element);
			}
			return tuple;
		}

		// key index is always stored in the first element of the vector
		BYTE get_key_index(std::vector<Element_In_Tuple> & in_update_tuple) {
			return in_update_tuple.at(0).key_index;
		}

		void set_key_index(std::vector<Element_In_Tuple> & out_update_tuple, int new_key_index) {
			out_update_tuple.at(0).key_index = new_key_index;
		}

		// TODO: do we need to store src.label?
		void build_edge_hashmap(char * edge_buf, std::vector<Element_In_Tuple> * edge_hashmap, size_t edge_file_size, int start_vertex) {

			// for each edge
			for(size_t pos = 0; pos < edge_file_size; pos += sizeof(LabeledEdge)) {
				// get a labeled edge
				LabeledEdge e = *(LabeledEdge*)(edge_buf + pos);
				assert(e.src >= start_vertex);
				// e.src is the key
				edge_hashmap[e.src - start_vertex].push_back(Element_In_Tuple(e.target, e.edge_label, e.target_label));
			}
		}

		int get_global_buffer_index(VertexId key) {

			int partition_id = key / context.num_vertices_per_part;
			return partition_id < (context.num_partitions - 1) ? partition_id : (context.num_partitions - 1);
		}

		bool is_automorphism(std::vector<Element_In_Tuple> sub_graph) {
			return false;
		}

		std::vector<Element_In_Tuple> & turn_canonical_graph(std::vector<Element_In_Tuple> & sub_graph) {

		}

		std::vector<Element_In_Tuple> & turn_quick_pattern(std::vector<Element_In_Tuple> & sub_graph) {

		}

	};
}



#endif /* CORE_MINING_PHASE_HPP_ */
