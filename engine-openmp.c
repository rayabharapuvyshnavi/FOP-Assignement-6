#define _FILE_OFFSET_BITS 64

#include "engine.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <omp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

struct chunk_result {
	struct count_result result;
	size_t capacity;
	int error;
};

static int count_matches(const char *line, const char *target,
						 size_t target_length) {
	int count = 0;
	const char *position = line;
	while ((position = strstr(position, target)) != NULL) {
		count++;
		position += target_length;
	}
	return count;
}

static char *trim_line(char *line) {
	while (isspace((unsigned char)*line)) {
		line++;
	}
	char *end = line + strlen(line);
	while (end > line && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
	return line;
}

static bool append_instance(struct chunk_result *chunk, const char *line) {
	struct count_result *result = &chunk->result;
	if ((size_t)result->instance_count == chunk->capacity) {
		size_t capacity = chunk->capacity == 0 ? 16 : chunk->capacity * 2;
		if (capacity > (size_t)INT_MAX) {
			capacity = INT_MAX;
		}
		char **instances = realloc(result->instances,
				capacity * sizeof(*result->instances));
		if (instances == NULL) {
			return false;
		}
		result->instances = instances;
		chunk->capacity = capacity;
	}

	char *copy = malloc(strlen(line) + 1);
	if (copy == NULL) {
		return false;
	}
	strcpy(copy, line);
	result->instances[result->instance_count++] = copy;
	return true;
}

static void free_result(struct count_result *result) {
	for (int i = 0; i < result->instance_count; i++) {
		free(result->instances[i]);
	}
	free(result->instances);
	*result = (struct count_result){0, 0, NULL};
}

static void search_instance_chunk(char *filename, char *target, off_t start,
								  off_t end, bool collect_instances,
								  struct chunk_result *chunk) {
	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		chunk->error = errno;
		return;
	}

	char *line = NULL;
	size_t line_capacity = 0;
	if (fseeko(file, start, SEEK_SET) != 0) {
		chunk->error = errno;
		goto cleanup;
	}
	if (start > 0) {
		if (fseeko(file, start - 1, SEEK_SET) != 0) {
			chunk->error = errno;
			goto cleanup;
		}
		int previous = fgetc(file);
		if (previous == EOF) {
			chunk->error = ferror(file) ? errno : EIO;
			goto cleanup;
		}
		if (previous != '\n' &&
				getline(&line, &line_capacity, file) < 0 && ferror(file)) {
			chunk->error = errno;
			goto cleanup;
		}
	}

	size_t target_length = strlen(target);
	while (true) {
		off_t line_start = ftello(file);
		if (line_start < 0) {
			chunk->error = errno;
			break;
		}
		if (line_start >= end) {
			break;
		}

		ssize_t length = getline(&line, &line_capacity, file);
		if (length < 0) {
			if (ferror(file)) {
				chunk->error = errno;
			}
			break;
		}

		char *search_line = collect_instances ? trim_line(line) : line;
		int matches = count_matches(search_line, target, target_length);
		if (matches > INT_MAX - chunk->result.count) {
			chunk->error = EOVERFLOW;
			break;
		}
		chunk->result.count += matches;

		if (collect_instances && matches > 0 &&
				chunk->result.instance_count == INT_MAX) {
			chunk->error = EOVERFLOW;
			break;
		}
		if (collect_instances && matches > 0 &&
				!append_instance(chunk, search_line)) {
			chunk->error = ENOMEM;
			break;
		}
	}

cleanup:
	free(line);
	if (fclose(file) != 0 && chunk->error == 0) {
		chunk->error = errno;
	}
}

static bool combine_results(struct chunk_result *chunks, int chunk_count,
							struct count_result *result) {
	size_t instance_count = 0;
	for (int i = 0; i < chunk_count; i++) {
		if (chunks[i].error != 0) {
			fprintf(stderr, "OpenMP search failed: %s\n",
					strerror(chunks[i].error));
			return false;
		}
		if (chunks[i].result.count > INT_MAX - result->count ||
				(size_t)chunks[i].result.instance_count >
				(size_t)INT_MAX - instance_count) {
			fprintf(stderr, "OpenMP search failed: result count overflow\n");
			return false;
		}
		result->count += chunks[i].result.count;
		instance_count += (size_t)chunks[i].result.instance_count;
	}

	if (instance_count == 0) {
		return true;
	}
	result->instances = calloc(instance_count, sizeof(*result->instances));
	if (result->instances == NULL) {
		perror("OpenMP search");
		return false;
	}

	for (int i = 0; i < chunk_count; i++) {
		for (int j = 0; j < chunks[i].result.instance_count; j++) {
			result->instances[result->instance_count++] =
					chunks[i].result.instances[j];
			chunks[i].result.instances[j] = NULL;
		}
	}
	return true;
}

static struct count_result search_file(char *filename, char *target,
									   bool collect_instances) {
	struct count_result result = {0, 0, NULL};
	if (filename == NULL || target == NULL || target[0] == '\0') {
		return result;
	}

	struct stat file_info;
	if (stat(filename, &file_info) != 0) {
		perror(filename);
		return result;
	}

	int thread_count = omp_get_max_threads();
	if (thread_count < 1) {
		thread_count = 1;
	}
	if (file_info.st_size > 0 &&
			(uintmax_t)file_info.st_size < (uintmax_t)thread_count) {
		thread_count = (int)file_info.st_size;
	}

	struct chunk_result *chunks =
			calloc((size_t)thread_count, sizeof(*chunks));
	if (chunks == NULL) {
		perror("OpenMP search");
		return result;
	}

	off_t base_size = file_info.st_size / thread_count;
	off_t extra_bytes = file_info.st_size % thread_count;
	#pragma omp parallel for num_threads(thread_count) schedule(static)
	for (int i = 0; i < thread_count; i++) {
		off_t index = (off_t)i;
		off_t start = index * base_size +
				(index < extra_bytes ? index : extra_bytes);
		off_t end = start + base_size + (index < extra_bytes ? 1 : 0);
		search_instance_chunk(filename, target, start, end, collect_instances,
							  &chunks[i]);
	}

	bool success = combine_results(chunks, thread_count, &result);
	for (int i = 0; i < thread_count; i++) {
		free_result(&chunks[i].result);
	}
	free(chunks);
	if (!success) {
		free_result(&result);
	}
	return result;
}

int search_count(char *filename, char *target) {
	return search_file(filename, target, false).count;
}

struct count_result search_instance(char *filename, char *target) {
	return search_file(filename, target, true);
}
