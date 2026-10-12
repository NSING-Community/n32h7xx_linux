/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <string.h>
extern const char *greeting(void);
int main(void)
{
    const char *message = greeting();
    puts(message);
    return strcmp(message, "PASS standard SDK shared library") != 0;
}
