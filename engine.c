#include "engine.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>

static ssize_t filereader(FILE *file, char **line, size_t *capacity) {
	return getline(line, capacity, file);
}

static int count_matches(const char *line, const char *target, size_t target_length) {
	int count = 0;
	const char *position = line;

	while ((position = strstr(position, target)) != NULL) {
		count++;
		position += target_length;
	}

	return count;
}

int search_count(char *filename, char *target) {
	if (filename == NULL || target == NULL || target[0] == '\0') {
		return 0;
	}

	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		return 0;
	}

	int count = 0;
	size_t target_length = strlen(target);
	char *line = NULL;
	size_t line_capacity = 0;
	while (filereader(file, &line, &line_capacity) != -1) {
		count += count_matches(line, target, target_length);
	}

	free(line);
	fclose(file);
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

static bool append_instance(struct count_result *result, size_t *capacity,
							const char *line) {
	if ((size_t)result->instance_count == *capacity) {
		size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
		char **instances = realloc(result->instances,
				new_capacity * sizeof(*result->instances));
		if (instances == NULL) {
			return false;
		}
		result->instances = instances;
		*capacity = new_capacity;
	}

	result->instances[result->instance_count] = malloc(strlen(line) + 1);
	if (result->instances[result->instance_count] == NULL) {
		return false;
	}
	strcpy(result->instances[result->instance_count], line);
	result->instance_count++;
	return true;
}

static void free_result(struct count_result *result) {
	for (int i = 0; i < result->instance_count; i++) {
		free(result->instances[i]);
	}
	free(result->instances);
	result->count = 0;
	result->instance_count = 0;
	result->instances = NULL;
}

struct count_result search_instance(char *filename, char *target) {
	struct count_result result = {0, 0, NULL};
	if (filename == NULL || target == NULL || target[0] == '\0') {
		return result;
	}

	FILE *file = fopen(filename, "r");
	if (file == NULL) {
		return result;
	}

	size_t instance_capacity = 0;
	size_t target_length = strlen(target);
	char *line = NULL;
	size_t line_capacity = 0;
	bool allocation_failed = false;

	while (filereader(file, &line, &line_capacity) != -1) {
		char *trimmed = trim_line(line);
		int matches = count_matches(trimmed, target, target_length);
		if (matches > 0) {
			result.count += matches;
			if (!append_instance(&result, &instance_capacity, trimmed)) {
				allocation_failed = true;
				break;
			}
		}
	}

	free(line);
	fclose(file);
	if (allocation_failed) {
		free_result(&result);
	}
	return result;
}