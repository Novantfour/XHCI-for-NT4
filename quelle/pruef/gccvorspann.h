/*  Nur fuer den Pruefbau mit gcc.  clang-cl kennt __int64 und
 *  __declspec von sich aus; gcc nicht.  Die Datei geht ueber -include
 *  hinein und steht in keinem Bau, der wirklich auf NT laeuft.  */
#define __int64        long long
#define __declspec(x)
