// stb_vorbis implementation, kept out of the runtime headers (their C macro clashes with stb names)
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"
