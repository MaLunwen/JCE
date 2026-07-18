#ifndef JCE_BINARY_EMBED_H
#define JCE_BINARY_EMBED_H

#include <cstddef>
#include <cstdint>
#include <string>

bool jce_binary_embed_write_c_source(const std::string &path,
                                     const std::string &symbol,
                                     const uint8_t *data, size_t size);
bool jce_binary_embed_write_coff(const std::string &path,
                                 const std::string &symbol,
                                 const uint8_t *data, size_t size,
                                 const std::string &arch);
bool jce_binary_embed_write_incbin(const std::string &path,
                                   const std::string &symbol,
                                   const std::string &input_path,
                                   size_t size);

#endif
