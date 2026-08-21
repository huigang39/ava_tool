#ifndef ELF_PARSER_H
#define ELF_PARSER_H
#include <stddef.h>
#include <stdint.h>
struct elf_parser;
int  elf_parser_open(struct elf_parser **out, const char *path);
void elf_parser_close(struct elf_parser *elf);
int  elf_parser_resolve(const struct elf_parser *elf, const char *expression, uint32_t *address);
int  elf_parser_resolve_info(const struct elf_parser *elf,
                             const char              *expression,
                             uint32_t                *address,
                             uint32_t                *size);
int  elf_parser_format(const struct elf_parser *elf,
                       const char              *expression,
                       const void              *data,
                       uint32_t                 size,
                       char                    *output,
                       size_t                   output_capacity);
int  elf_parser_encode(const struct elf_parser *elf,
                       const char              *expression,
                       const char              *text,
                       void                    *output,
                       uint32_t                 output_size);
#endif
