/*
 * scatter.hpp
 *
 *  Created on: Mar 16, 2017
 *      Author: kai
 */

#ifndef CORE_SCATTER_HPP_
#define CORE_SCATTER_HPP_

#include "io_manager.hpp"
#include "buffer_manager.hpp"
#include "concurrent_queue.hpp"
#include "type.hpp"
#include "constants.hpp"

namespace RStream {
	template <typename VertexDataType, typename UpdateType>
	class Scatter {
		const engine<VertexDataType, UpdateType>& context;

		std::atomic<int> atomic_num_producers;
		std::atomic<int> atomic_partition_id;
		std::atomic<int> atomic_partition_number;

	public:

		Scatter(engine<VertexDataType, UpdateType> & e) : context(e) {};

		/* scatter with vertex data (for graph computation use)*/
		void scatter_with_vertex(std::function<T*(Edge&, char*)> generate_one_update) {
			// a pair of <vertex, edge_stream> for each partition
			concurrent_queue<std::pair<int, int>> * task_queue = new concurrent_queue<std::pair<int, int>>(context.num_partitions);

			// push task into concurrent queue
			for(int partition_id = 0; partition_id < context.num_partitions; partition_id++) {
				int fd_vertex = open((context.filename + "." + std::to_string(partition_id) + ".vertex").c_str(), O_RDONLY);
				int fd_edge = open((context.filename + "." + std::to_string(partition_id)).c_str(), O_RDONLY);
				assert(fd_vertex > 0 && fd_edge > 0);
				task_queue->push(std::make_pair(fd_vertex, fd_edge));
			}

			// allocate global buffers for shuffling
			global_buffer<T> ** buffers_for_shuffle = buffer_manager<T>::get_global_buffers(context.num_partitions);

			// exec threads will produce updates and push into shuffle buffers
			std::vector<std::thread> exec_threads;
			for(int i = 0; i < context.num_partitions; i++)
				exec_threads.push_back( std::thread([=] { this->scatter_producer_with_vertex(generate_one_update, buffers_for_shuffle, task_queue); } ));

			// write threads will flush shuffle buffer to update out stream file as long as it's full
			std::vector<std::thread> write_threads;
			for(int i = 0; i < context.num_write_threads; i++)
				write_threads.push_back(std::thread(&Scatter::scatter_consumer, this, buffers_for_shuffle));

			// join all threads
			for(auto & t : exec_threads)
				t.join();

			for(auto &t : write_threads)
				t.join();

			delete[] buffers_for_shuffle;
		}

		/* scatter without vertex data (for relational algebra use)*/
		void scatter_no_vertex(std::function<T*(Edge&)> generate_one_update) {
			concurrent_queue<int> * task_queue = new concurrent_queue<int>(context.num_partitions);

			// allocate global buffers for shuffling
			global_buffer<T> ** buffers_for_shuffle = buffer_manager<T>::get_global_buffers(context.num_partitions);

			// push task into concurrent queue
			for(int partition_id = 0; partition_id < context.num_partitions; partition_id++) {
				int fd = open((context.filename + "." + std::to_string(partition_id)).c_str(), O_RDONLY);
				assert(fd > 0);
				task_queue->push(fd);
			}

//			//for debugging only
//			scatter_producer(generate_one_update, buffers_for_shuffle, task_queue);
//			std::cout << "scatter done!" << std::endl;
//			scatter_consumer(buffers_for_shuffle);

			// exec threads will produce updates and push into shuffle buffers
			std::vector<std::thread> exec_threads;
			for(int i = 0; i < context.num_exec_threads; i++)
				exec_threads.push_back(std::thread([=] { this->scatter_producer_no_vertex(generate_one_update, buffers_for_shuffle, task_queue); }));


			// write threads will flush shuffle buffer to update out stream file as long as it's full
			std::vector<std::thread> write_threads;
			for(int i = 0; i < context.num_write_threads; i++)
				write_threads.push_back(std::thread(&Scatter::scatter_consumer, this, buffers_for_shuffle));

			// join all threads
			for(auto & t : exec_threads)
				t.join();

			for(auto &t : write_threads)
				t.join();

			delete[] buffers_for_shuffle;
		}

	private:
		/* scatter producer with vertex data*/
		//each exec thread generates a scatter_producer
		void scatter_producer_with_vertex(std::function<T*(Edge&, char*)> generate_one_update,
				global_buffer<T> ** buffers_for_shuffle, concurrent_queue<std::pair<int, int>> * task_queue) {

			atomic_num_producers++;
			std::pair<int, int> fd_pair(-1, -1);
			// pop from queue
			while(task_queue->test_pop_atomic(fd_pair)){
				int fd_vertex = fd_pair.first;
				int fd_edge = fd_pair.second;
				assert(fd_vertex > 0 && fd_edge > 0 );

				// get file size
				size_t vertex_file_size = io_manager::get_filesize(fd_vertex);
				size_t edge_file_size = io_manager::get_filesize(fd_edge);

				// read from files to thread local buffer
				char * vertex_local_buf = new char[vertex_file_size];
				io_manager::read_from_file(fd_vertex, vertex_local_buf, vertex_file_size);
				char * edge_local_buf = new char[edge_file_size];
				io_manager::read_from_file(fd_edge, edge_local_buf, edge_file_size);

				// for each edge
				for(size_t pos = 0; pos < edge_file_size; pos += context.edge_unit) {
					// get an edge
					Edge e = *(Edge*)(edge_local_buf + pos);
//					std::cout << e << std::endl;

					// gen one update
					T * update_info = generate_one_update(e, vertex_local_buf);
//					std::cout << update_info->target << std::endl;


					// insert into shuffle buffer accordingly
					int index = get_global_buffer_index(update_info);
					global_buffer<T>* global_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, context.num_partitions, index);
					global_buf->insert(update_info, index);
				}

//				std::cout << std::endl;

				// delete
				delete[] vertex_local_buf;
				delete[] edge_local_buf;
				close(fd_vertex);
				close(fd_edge);

			}
			atomic_num_producers--;

		}

		/* scatter producer without vertex data*/
		// each exec thread generates a scatter_producer
		void scatter_producer_no_vertex(std::function<T*(Edge&)> generate_one_update,
				global_buffer<T> ** buffers_for_shuffle, concurrent_queue<int> * task_queue) {
			atomic_num_producers++;
			int fd = -1;
			// pop from queue
			while(task_queue->test_pop_atomic(fd)){
				assert(fd > 0);

				// get file size
				size_t file_size = io_manager::get_filesize(fd);
				print_thread_info_locked("as a producer dealing with " + std::to_string(fd) + " of size " + std::to_string(file_size) + "\n");

				// read from file to thread local buffer
				char * local_buf = new char[file_size];
				io_manager::read_from_file(fd, local_buf, file_size);

				// for each edge
				for(size_t pos = 0; pos < file_size; pos += context.edge_unit) {
					// get an edge
					Edge e = *(Edge*)(local_buf + pos);
//					std::cout << e << std::endl;

					// gen one update
					T * update_info = generate_one_update(e);
//					std::cout << update_info->target << std::endl;


					// insert into shuffle buffer accordingly
					int index = get_global_buffer_index(update_info);
					global_buffer<T>* global_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, context.num_partitions, index);
					global_buf->insert(update_info, index);
				}

//				std::cout << std::endl;
				delete[] local_buf;
				close(fd);

			}
			atomic_num_producers--;

		}

		// each writer thread generates a scatter_consumer
		void scatter_consumer(global_buffer<T> ** buffers_for_shuffle) {
			while(atomic_num_producers != 0) {
				int i = (atomic_partition_id++) % context.num_partitions ;

//				//debugging info
//				print_thread_info("as a consumer dealing with buffer[" + std::to_string(i) + "]\n");

				const char * file_name = (context.filename + "." + std::to_string(i) + ".update_stream").c_str();
				global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, context.num_partitions, i);
				g_buf->flush(file_name, i);
			}

			//the last run - deal with all remaining content in buffers
			while(true){
				int i = atomic_partition_number--;
//				std::cout << i << std::endl;
				if(i >= 0){
//					//debugging info
//					print_thread_info("as a consumer dealing with buffer[" + std::to_string(i) + "]\n");

					const char * file_name = (context.filename + "." + std::to_string(i) + ".update_stream").c_str();
					global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, context.num_partitions, i);
					g_buf->flush_end(file_name, i);

					delete g_buf;
				}
				else{
					break;
				}
			}
		}

		int get_global_buffer_index(T* update_info) {
//			return update_info->target;
			return 0;
		}

	};


}



#endif /* CORE_SCATTER_HPP_ */
