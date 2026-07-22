/* filename: LongTermExtractor.h — the example module from docs/Design.md §9 */
#ifndef LONG_TERM_EXTRACTOR_H_
#define LONG_TERM_EXTRACTOR_H_

struct LongTermExtractor;

struct LongTermExtractor *long_term_extractor_init(int trader_id, int value);
int long_term_extractor_close(struct LongTermExtractor *p_lte);

#endif
