/*
 * preprocessing_new.hpp
 *
 *  Created on: Jul 26, 2017
 *      Author: kai
 */

#ifndef UTILITY_PREPROCESSING_NEW_HPP_
#define UTILITY_PREPROCESSING_NEW_HPP_

#include <iostream>
#include <climits>

namespace RStream {
	class Preprocessing_new {
		std::string input;
		int format;

		VertexId minVertexId;
		VertexId maxVertexId;

		int numPartitions;
		int numVertices;
		int vertices_per_partition;

		int edgeType;
		int edge_unit;

		std::atomic<int> atomic_num_producers;
		std::atomic<int> atomic_partition_number;
		int num_exec_threads;
		int num_write_threads;

	public:
		Preprocessing_new(std::string & _input, int _num_partitioins, int _format) : input(_input), format(_format),minVertexId(INT_MAX), maxVertexId(INT_MIN),
			numPartitions(_num_partitioins), numVertices(0), vertices_per_partition(0), edgeType(0), edge_unit(0){
			num_exec_threads = 3;
			num_write_threads = 1;

			atomic_init();
			run();
		}

		void atomic_init() {
			atomic_num_producers = num_exec_threads;
			atomic_partition_number = numPartitions;
		}

		void run() {
			std::cout << "start preprocessing..." << std::endl;

			// convert txt to binary
			if(format == (int)FORMAT::EdgeList) {
				std::cout << "start to convert edge list file..." << std::endl;
				convert_edgelist();
				std::cout << "convert edge list file done." << std::endl;
				if(edgeType == (int)EdgeType::NO_WEIGHT) {
					std::cout << "start to partition on vertices..." << std::endl;
					partition_on_vertices<Edge>();
					std::cout << "partition on vertices done." << std::endl;
				}

				std::cout << "gen partition done!" << std::endl;
				write_meta_file();

			} else if(format == (int)FORMAT::AdjList) {
				std::cout << "start to convert adj list file..." << std::endl;
				convert_adjlist();
			}

		}

		// note: vertex id always starts with 0
		void convert_edgelist() {
			FILE * fd = fopen(input.c_str(), "r");
			assert(fd != NULL );
			char * buf = (char *)memalign(PAGE_SIZE, IO_SIZE);
			long pos = 0;

			long counter = 0;
			char s[1024];
			while(fgets(s, 1024, fd) != NULL) {
				if (s[0] == '#') continue; // Comment
				if (s[0] == '%') continue; // Comment

				char delims[] = "\t, ";
				char * t;
				t = strtok(s, delims);
				assert(t != NULL);

				VertexId from = atoi(t);
				t = strtok(NULL, delims);
				assert(t != NULL);
				VertexId to = atoi(t);

				minVertexId = std::min(minVertexId, from);
				minVertexId = std::min(minVertexId, to);
				maxVertexId = std::max(maxVertexId, from);
				maxVertexId = std::max(maxVertexId, to);
			}
			numVertices = maxVertexId - minVertexId + 1;

			fclose(fd);
			fd = fopen(input.c_str(), "r");
			while(fgets(s, 1024, fd) != NULL) {
				if (s[0] == '#') continue; // Comment
				if (s[0] == '%') continue; // Comment

				char delims[] = "\t, ";
				char * t;
				t = strtok(s, delims);
				assert(t != NULL);

				VertexId from = atoi(t);
				from -= minVertexId;
				t = strtok(NULL, delims);
				assert(t != NULL);
				VertexId to = atoi(t);
				to -= minVertexId;

				if(from == to) continue;

				void * data = nullptr;
				Weight val;
				/* Check if has value */
				t = strtok(NULL, delims);
				if(t != NULL) {
					val = atof(t);
					data = new WeightedEdge(from, to, val);
					edge_unit = sizeof(VertexId) * 2 + sizeof(Weight);
					std::memcpy(buf + pos, data, edge_unit);
					pos += edge_unit;
					edgeType = (int)EdgeType::WITH_WEIGHT;
				} else {
					data = new Edge(from, to);
					edge_unit =  sizeof(VertexId) * 2;
					std::memcpy(buf + pos, data, edge_unit);
					pos += edge_unit;
					edgeType = (int)EdgeType::NO_WEIGHT;
				}
				counter++;

				assert(IO_SIZE % edge_unit == 0);
				if(pos >= IO_SIZE) {
					int perms = O_WRONLY | O_APPEND;
					int fout = open((input + ".binary").c_str(), perms, S_IRWXU);
					if(fout < 0){
						fout = creat((input + ".binary").c_str(), S_IRWXU);
					}
					io_manager::write_to_file(fout, buf, IO_SIZE);
					counter -= IO_SIZE / edge_unit;
					close(fout);
					pos = 0;
				}

			}

			int perms = O_WRONLY | O_APPEND;
			int fout = open((input + ".binary").c_str(), perms, S_IRWXU);
			if(fout < 0){
				fout = creat((input + ".binary").c_str(), S_IRWXU);
			}

			io_manager::write_to_file(fout, buf, counter * edge_unit);
			close(fout);
			free(buf);

			fclose(fd);
		};

		void convert_adjlist() {

		};

		template<typename T>
		void partition_on_vertices() {
			vertices_per_partition = numVertices / numPartitions;

			int fd = open((input + ".binary").c_str(), O_RDONLY);
			assert(fd > 0 );

			// get file size
			long file_size = io_manager::get_filesize(fd);
			int streaming_counter = file_size / IO_SIZE + 1;
			long valid_io_size = 0;
			long offset = 0;

			concurrent_queue<std::tuple<int, long, long>> * task_queue = new concurrent_queue<std::tuple<int, long, long>>(65536);
			// <fd, offset, length>
			for(int counter = 0; counter < streaming_counter; counter++) {
				if(counter == streaming_counter - 1)
					// TODO: potential overflow?
					valid_io_size = file_size - IO_SIZE * (streaming_counter - 1);
				else
					valid_io_size = IO_SIZE;

				task_queue->push(std::make_tuple(fd, offset, valid_io_size));
				offset += valid_io_size;
			}

			global_buffer<T> ** buffers_for_shuffle = buffer_manager<T>::get_global_buffers(numPartitions);

			std::vector<std::thread> exec_threads;
			for(int i = 0; i < num_exec_threads; i++)
				exec_threads.push_back( std::thread([=] { this->producer<T>(buffers_for_shuffle, task_queue); } ));

			std::vector<std::thread> write_threads;
			for(int i = 0; i < num_write_threads; i++)
				write_threads.push_back(std::thread(&Preprocessing_new::consumer<T>, this, buffers_for_shuffle));

			// join all threads
			for(auto & t : exec_threads)
				t.join();

			for(auto &t : write_threads)
				t.join();

			delete[] buffers_for_shuffle;
			delete task_queue;
			close(fd);

		};

		void partition_on_edges() {

		};

		void write_meta_file() {
			std::ofstream meta_file(input + ".meta");
			if(meta_file.is_open()) {
				meta_file << edgeType << "\t" << edge_unit << "\n";
				meta_file << numVertices << "\t" << vertices_per_partition << "\n";

				VertexId start = 0, end = 0;
				for(int i = 0; i < numPartitions; i++) {
					// last partition
					if(i == numPartitions - 1) {
						end = start + numVertices - vertices_per_partition * (numPartitions - 1) - 1;
						meta_file << start << "\t" << end << "\n";
					} else {
						end = start + vertices_per_partition - 1;
						meta_file << start << "\t" << end << "\n";
						start = end + 1;
					}
				}

			} else {
				std::cout << "Could not open meta file!";
			}

			meta_file.close();
		}


	private:
		template<typename T>
		void producer(global_buffer<T> ** buffers_for_shuffle, concurrent_queue<std::tuple<int, long, long> > * task_queue) {
			int fd = -1;
			long offset = 0, length = 0;
			auto one_task = std::make_tuple(fd, offset, length);

			char * local_buf = (char *)memalign(PAGE_SIZE, IO_SIZE);
			VertexId src = 0, dst = 0;
			Weight weight = 0.0f;

			// pop from queue
			while(task_queue->test_pop_atomic(one_task)){
				fd = std::get<0>(one_task);
				offset = std::get<1>(one_task);
				length = std::get<2>(one_task);

				io_manager::read_from_file(fd, local_buf, length, offset);
				for(long pos = 0; pos < length; pos += edge_unit) {
					src = *(VertexId*)(local_buf + pos);
					dst = *(VertexId*)(local_buf + pos + sizeof(VertexId));
					assert(src >= 0 && src < numVertices && dst >= 0 && dst < numVertices);

					void * data = nullptr;
					if(typeid(T) == typeid(Edge)) {
						data = new Edge(src, dst);
					} else if(typeid(T) == typeid(WeightedEdge)) {
						weight = *(Weight*)(local_buf + pos + sizeof(VertexId) * 2);
						data = new WeightedEdge(src, dst, weight);
					}

					int index = get_index_partition_vertices(src);

					global_buffer<T>* global_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, numPartitions, index);
					global_buf->insert((T*)data, index);

				}

			}

			free(local_buf);
			atomic_num_producers--;
		}

		template<typename T>
		void consumer(global_buffer<T> ** buffers_for_shuffle) {
			int counter = 0;

			while(atomic_num_producers != 0) {
				if(counter == numPartitions)
					counter = 0;

				int i = counter++;
				std::string file_name = (input + "." + std::to_string(i));
				global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, numPartitions, i);
				g_buf->flush(file_name, i);
			}

			//the last run - deal with all remaining content in buffers
			while(true){
				int i = --atomic_partition_number;
				if(i >= 0){
					std::string file_name_str = (input + "." + std::to_string(i));
					global_buffer<T>* g_buf = buffer_manager<T>::get_global_buffer(buffers_for_shuffle, numPartitions, i);
					g_buf->flush_end(file_name_str, i);

					delete g_buf;
				}
				else{
					break;
				}
			}
		}

		int get_index_partition_vertices(int src) {

			int partition_id = src/ vertices_per_partition;
			return partition_id < (numPartitions - 1) ? partition_id : (numPartitions - 1);
		}

	};
}



#endif /* UTILITY_PREPROCESSING_NEW_HPP_ */
