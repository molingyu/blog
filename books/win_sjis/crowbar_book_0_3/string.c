#include <stdio.h>
#include <string.h>
#include "MEM.h"
#include "crowbar.h"

#define STRING_ALLOC_SIZE       (256)

static char *st_string_literal_buffer = NULL;
static int st_string_literal_buffer_size = 0;
static int st_string_literal_buffer_alloc_size = 0;

void
crb_open_string_literal(void)
{
    st_string_literal_buffer_size = 0;
}

void
crb_add_string_literal(int letter)
{
    if (st_string_literal_buffer_size == st_string_literal_buffer_alloc_size) {
        st_string_literal_buffer_alloc_size += STRING_ALLOC_SIZE;
        st_string_literal_buffer
            = MEM_realloc(st_string_literal_buffer,
                          st_string_literal_buffer_alloc_size);
    }
    st_string_literal_buffer[st_string_literal_buffer_size] = letter;
    st_string_literal_buffer_size++;
}

void
crb_reset_string_literal_buffer(void)
{
    MEM_free(st_string_literal_buffer);
    st_string_literal_buffer = NULL;
    st_string_literal_buffer_size = 0;
    st_string_literal_buffer_alloc_size = 0;
}

CRB_Char *
crb_close_string_literal(void)
{
    CRB_Char *new_str;
    int new_str_len;

    crb_add_string_literal('\0');
    new_str_len = CRB_mbstowcs_len(st_string_literal_buffer);
    if (new_str_len < 0) {
        crb_compile_error(BAD_MULTIBYTE_CHARACTER_IN_COMPILE_ERR,
                          MESSAGE_ARGUMENT_END);
    }
    new_str = crb_malloc(sizeof(CRB_Char) * (new_str_len+1));
    CRB_mbstowcs(st_string_literal_buffer, new_str);

    return new_str;
}

char *
crb_create_identifier(char *str)
{
    char *new_str;

    new_str = crb_malloc(strlen(str) + 1);

    strcpy(new_str, str);

    return new_str;
}

