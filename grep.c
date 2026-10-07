#include "engine.h"
#include <stdio.h>
#include <string.h>
#i

int main(int argc, char** argv) {
    // TODO: parse the arguments in argv.
    // You can expect argv[1] to be the mode
    // You can expect argv[2] to be the filepath
    // You can expect argv[3] to be the target word

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <count|instance> <input_file> <target_word>\n",
                argv[0]);
        return 1;
    }

    char * mode = argv[1];
    char * filepath = argv[2];
    char * target_word = argv[3];


    if (strcmp(mode,"count") ==0){
        int count = search_count(filepath, target_word);
        printf("Found: %d of %s in %s", count, target_word, filepath);
        
    }
    else if (strcmp(mode , "instance") == 0){
        struct count_result result = search_instance(filepath, target_word);
        // Found: 2 of the in data/small.txt
        // res.instances[0]: the quick
        // res.instances[1]: the lazy dog
        
        printf("Found: %d of %s in %s", result.count, target_word, filepath);

        for(int i = 0 ; i < result.count ; i++){
            printf("res.instances[%d]: %s", i, result.instances[i]);
        }
        
    }

    return 0;
}