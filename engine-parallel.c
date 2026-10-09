#define _FILE_OFFSET_BITS 64

#include "engine.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct instance_entry {
	off_t line_start;
	char *line;
};

struct shared_instance_result {
	struct count_result result;
	struct instance_entry *entries;
	size_t capacity;
	pthread_mutex_t mutex;
};

struct worker_args {
	char *filename;
	char *target;
	off_t start;
	off_t end;
	int *shared_count;
	pthread_mutex_t *count_mutex;
	struct shared_instance_result *shared_instances;
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

static size_t get_worker_count(off_t file_size) {
	long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	size_t count = cpu_count > 0 ? (size_t)cpu_count : 1;
	if (file_size > 0 && (uintmax_t)file_size < count) {
		count = (size_t)file_size;
	}
	return count == 0 ? 1 : count;
}

static bool prepare_workers(char *filename, char *target, off_t file_size,
							size_t *worker_count, struct worker_args **args_out,
							pthread_t **threads_out) {
	*worker_count = get_worker_count(file_size);
	struct worker_args *args = calloc(*worker_count, sizeof(*args));
	pthread_t *threads = calloc(*worker_count, sizeof(*threads));
	if (args == NULL || threads == NULL) {
		free(args);
		free(threads);
		errno = ENOMEM;
		return false;
	}

	off_t base_size = file_size / (off_t)*worker_count;
	off_t extra_bytes = file_size % (off_t)*worker_count;
	for (size_t i = 0; i < *worker_count; i++) {
		off_t index = (off_t)i;
		args[i].filename = filename;
		args[i].target = target;
		args[i].start = index * base_size +
				(index < extra_bytes ? index : extra_bytes);
		args[i].end = args[i].start + base_size +
				(index < extra_bytes ? 1 : 0);
	}

	*args_out = args;
	*threads_out = threads;
	return true;
}

static bool seek_chunk_start(FILE *file, struct worker_args *args,
							 char **line, size_t *line_capacity) {
	if (fseeko(file, args->start, SEEK_SET) != 0) {
		args->error = errno;
		return false;
	}

	if (args->start > 0) {
		if (fseeko(file, args->start - 1, SEEK_SET) != 0) {
			args->error = errno;
			return false;
		}
		int previous = fgetc(file);
		if (previous == EOF) {
			args->error = ferror(file) ? errno : EIO;
			return false;
		}
		if (previous != '\n' &&
				getline(line, line_capacity, file) < 0 && ferror(file)) {
			args->error = errno;
			return false;
		}
	}

	return true;
}

static FILE *open_chunk(struct worker_args *args, char **line,
						size_t *line_capacity) {
	FILE *file = fopen(args->filename, "r");
	if (file == NULL) {
		args->error = errno;
		return NULL;
	}
	if (!seek_chunk_start(file, args, line, line_capacity)) {
		fclose(file);
		return NULL;
	}
	return file;
}

static void *count_worker(void *argument) {
	struct worker_args *args = argument;
	char *line = NULL;
	size_t line_capacity = 0;
	size_t target_length = strlen(args->target);
	int local_count = 0;
	FILE *file = open_chunk(args, &line, &line_capacity);
	if (file == NULL) {
		free(line);
		return NULL;
	}

	while (true) {
		off_t line_start = ftello(file);
		if (line_start < 0) {
			args->error = errno;
			break;
		}
		if (line_start >= args->end) {
			break;
		}

		ssize_t length = getline(&line, &line_capacity, file);
		if (length < 0) {
			if (ferror(file)) {
				args->error = errno;
			}
			break;
		}
		int matches = count_matches(line, args->target, target_length);
		if (matches > INT_MAX - local_count) {
			args->error = EOVERFLOW;
			break;
		}
		local_count += matches;
	}

	free(line);
	fclose(file);

	if (args->error == 0) {
		int error = pthread_mutex_lock(args->count_mutex);
		if (error != 0) {
			args->error = error;
			return NULL;
		}
		if (local_count > INT_MAX - *args->shared_count) {
			args->error = EOVERFLOW;
		} else {
			*args->shared_count += local_count;
		}
		error = pthread_mutex_unlock(args->count_mutex);
		if (error != 0) {
			args->error = error;
		}
	}
	return NULL;
}

static bool append_shared_instance(struct worker_args *args, off_t line_start,
								   const char *line, int matches) {
	struct shared_instance_result *shared = args->shared_instances;
	int error = pthread_mutex_lock(&shared->mutex);
	if (error != 0) {
		args->error = error;
		return false;
	}

	bool success = false;
	if (matches > INT_MAX - shared->result.count ||
			shared->result.instance_count == INT_MAX) {
		args->error = EOVERFLOW;
	} else {
		if ((size_t)shared->result.instance_count == shared->capacity) {
			size_t new_capacity = shared->capacity == 0
					? 16 : shared->capacity * 2;
			if (new_capacity > (size_t)INT_MAX) {
				new_capacity = INT_MAX;
			}
			struct instance_entry *entries = realloc(shared->entries,
					new_capacity * sizeof(*shared->entries));
			if (entries == NULL) {
				args->error = ENOMEM;
			} else {
				shared->entries = entries;
				shared->capacity = new_capacity;
			}
		}

		if (args->error == 0) {
			char *copy = malloc(strlen(line) + 1);
			if (copy == NULL) {
				args->error = ENOMEM;
			} else {
				strcpy(copy, line);
				size_t index = (size_t)shared->result.instance_count;
				shared->entries[index].line_start = line_start;
				shared->entries[index].line = copy;
				shared->result.instance_count++;
				shared->result.count += matches;
				success = true;
			}
		}
	}

	error = pthread_mutex_unlock(&shared->mutex);
	if (error != 0) {
		args->error = error;
		return false;
	}
	return success;
}

static void *instance_worker(void *argument) {
	struct worker_args *args = argument;
	char *line = NULL;
	size_t line_capacity = 0;
	size_t target_length = strlen(args->target);
	FILE *file = open_chunk(args, &line, &line_capacity);
	if (file == NULL) {
		free(line);
		return NULL;
	}

	while (true) {
		off_t line_start = ftello(file);
		if (line_start < 0) {
			args->error = errno;
			break;
		}
		if (line_start >= args->end) {
			break;
		}

		ssize_t length = getline(&line, &line_capacity, file);
		if (length < 0) {
			if (ferror(file)) {
				args->error = errno;
			}
			break;
		}

		char *trimmed = trim_line(line);
		int matches = count_matches(trimmed, args->target, target_length);
		if (matches > 0 &&
				!append_shared_instance(args, line_start, trimmed, matches)) {
			break;
		}
	}

	free(line);
	fclose(file);
	return NULL;
}

static void report_worker_errors(const struct worker_args *args,
								size_t worker_count) {
	for (size_t i = 0; i < worker_count; i++) {
		if (args[i].error != 0) {
			fprintf(stderr, "parallel search failed: %s\n",
					strerror(args[i].error));
		}
	}
}

static bool join_workers(pthread_t *threads, size_t started) {
	bool success = true;
	for (size_t i = 0; i < started; i++) {
		int error = pthread_join(threads[i], NULL);
		if (error != 0) {
			fprintf(stderr, "pthread_join failed: %s\n", strerror(error));
			success = false;
		}
	}
	return success;
}

static bool launch_workers(struct worker_args *args, pthread_t *threads,
						   size_t worker_count, void *(*worker)(void *),
						   size_t *started) {
	*started = 0;
	for (size_t i = 0; i < worker_count; i++) {
		int error = pthread_create(&threads[i], NULL, worker, &args[i]);
		if (error != 0) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(error));
			return false;
		}
		(*started)++;
	}
	return true;
}

static int compare_instance_entries(const void *left, const void *right) {
	const struct instance_entry *a = left;
	const struct instance_entry *b = right;
	return (a->line_start > b->line_start) - (a->line_start < b->line_start);
}

int search_count(char *filename, char *target) {
	if (filename == NULL || target == NULL || target[0] == '\0') {
		return 0;
	}

	struct stat file_info;
	if (stat(filename, &file_info) != 0) {
		perror(filename);
		return 0;
	}

	size_t worker_count;
	struct worker_args *args;
	pthread_t *threads;
	if (!prepare_workers(filename, target, file_info.st_size, &worker_count,
						 &args, &threads)) {
		perror("parallel search");
		return 0;
	}

	int shared_count = 0;
	pthread_mutex_t count_mutex;
	int error = pthread_mutex_init(&count_mutex, NULL);
	if (error != 0) {
		fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(error));
		free(args);
		free(threads);
		return 0;
	}
	for (size_t i = 0; i < worker_count; i++) {
		args[i].shared_count = &shared_count;
		args[i].count_mutex = &count_mutex;
	}

	size_t started;
	bool created = launch_workers(args, threads, worker_count, count_worker,
								  &started);
	bool joined = join_workers(threads, started);
	report_worker_errors(args, worker_count);

	bool success = created && joined;
	for (size_t i = 0; i < worker_count; i++) {
		if (args[i].error != 0) {
			success = false;
		}
	}

	error = pthread_mutex_destroy(&count_mutex);
	if (error != 0) {
		fprintf(stderr, "pthread_mutex_destroy failed: %s\n", strerror(error));
		success = false;
	}
	free(args);
	free(threads);
	return success ? shared_count : 0;
}

struct count_result search_instance(char *filename, char *target) {
	struct count_result empty_result = {0, 0, NULL};
	if (filename == NULL || target == NULL || target[0] == '\0') {
		return empty_result;
	}

	struct stat file_info;
	if (stat(filename, &file_info) != 0) {
		perror(filename);
		return empty_result;
	}

	struct shared_instance_result shared = {0};
	int error = pthread_mutex_init(&shared.mutex, NULL);
	if (error != 0) {
		fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(error));
		return empty_result;
	}

	size_t worker_count;
	struct worker_args *args;
	pthread_t *threads;
	if (!prepare_workers(filename, target, file_info.st_size, &worker_count,
						 &args, &threads)) {
		perror("parallel search");
		pthread_mutex_destroy(&shared.mutex);
		return empty_result;
	}
	for (size_t i = 0; i < worker_count; i++) {
		args[i].shared_instances = &shared;
	}

	size_t started;
	bool created = launch_workers(args, threads, worker_count, instance_worker,
								  &started);
	bool joined = join_workers(threads, started);
	report_worker_errors(args, worker_count);

	bool success = created && joined;
	for (size_t i = 0; i < worker_count; i++) {
		if (args[i].error != 0) {
			success = false;
		}
	}

	struct count_result result = empty_result;
	if (success && shared.result.instance_count > 0) {
		qsort(shared.entries, (size_t)shared.result.instance_count,
			  sizeof(*shared.entries), compare_instance_entries);
		result.instances = calloc((size_t)shared.result.instance_count,
								  sizeof(*result.instances));
		if (result.instances == NULL) {
			perror("parallel search");
			success = false;
		} else {
			result.count = shared.result.count;
			result.instance_count = shared.result.instance_count;
			for (int i = 0; i < result.instance_count; i++) {
				result.instances[i] = shared.entries[i].line;
				shared.entries[i].line = NULL;
			}
		}
	} else if (success) {
		result.count = shared.result.count;
	}

	if (!success) {
		for (int i = 0; i < shared.result.instance_count; i++) {
			free(shared.entries[i].line);
		}
		free(result.instances);
		result = empty_result;
	}
	free(shared.entries);
	free(args);
	free(threads);

	error = pthread_mutex_destroy(&shared.mutex);
	if (error != 0) {
		fprintf(stderr, "pthread_mutex_destroy failed: %s\n", strerror(error));
		for (int i = 0; i < result.instance_count; i++) {
			free(result.instances[i]);
		}
		free(result.instances);
		return empty_result;
	}
	return result;
}
