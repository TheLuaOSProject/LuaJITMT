from pathlib import Path
p=Path(__file__).parent;f=p/'t-huge-tail.c';s=f.read_text();start=s.index('static void geometry(')
s=s[:start]+'''static void payload_tail_probe(PRNGState *rs) {
 size_t size=65393;void *p=lj_arena_huge_map(rs,size,LJ_AF_TRAVERSABLE);la_u128 proof;
 assert(p);watch(p,size);proof=wide_seed(p);memset(p,0xa7,size);
 /* Causal negative: the old mapping length lets the final user byte overwrite W. */
 wide_is(p,proof);bytes_are(p,size,0xa7);lj_arena_huge_unmap(p,size);
 puts("final advertised payload byte leaves W intact");
}
''' + s[start:]
s=s.replace('if(!strcmp(mode,"all") || !strcmp(mode,"geometry"))geometry(&rs);','if(!strcmp(mode,"all") || !strcmp(mode,"geometry"))geometry(&rs);\n if(!strcmp(mode,"all") || !strcmp(mode,"payload"))payload_tail_probe(&rs);')
f.write_text(s)
f=p/'targeted.py';s=f.read_text().replace("['geometry','bounds','resize','failures']","['geometry','payload','bounds','resize','failures']");f.write_text(s)
f=p/'validate.py';s=f.read_text().replace(" if name=='overflow':cmd+=['-DDENSE_WRAP_CALLOC','-Wl,--wrap=calloc']"," if name=='overflow':cmd+=['-DDENSE_WRAP_CALLOC','-Wl,--wrap=calloc']\n if name=='terminal-orphan':cmd+=['-Wl,--wrap=lj_arena_hugetab_transfer','-Wl,--wrap=munmap']");f.write_text(s)
