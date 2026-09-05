from pathlib import Path
import subprocess,tarfile,shutil,json,hashlib,difflib
p=Path(__file__).parent;base='28de50a622e489019fa22845d6454e029b210582';t=p/'calloc';parent=Path('/tmp/lj-dense-overflow-20260905-7tl6kcfk/dense-W-candidate.patch')
q=subprocess.run(['git','apply','--exclude=src/lj_arena.c',str(parent)],cwd=t,capture_output=True,text=True);(p/'apply-except-arena.log').write_text(q.stdout+q.stderr);assert q.returncode==0
f=t/'src/lj_arena.c';s=f.read_text()
old='''  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);
  return (void *)((char *)a + sizeof(GCAhdr));'''
new='''  a->hdr.live_cells = (uint32_t)(mapsize >> LJ_CELL_SHIFT);
  if (flags & LJ_AF_TRAVERSABLE) {
    LJGC2TabWideStamp *wide = NULL;
    if (!arena_test_gc2_sidecar_alloc_fails())
      wide = (LJGC2TabWideStamp *)calloc(1, sizeof(*wide));
    if (!wide) {
      arena_unmap_aligned(a, mapsize);
      return NULL;  /* No mapping, header or body has been published. */
    }
    la_storeptr_rel((void **)&a->hdr.gc2_huge_wide, wide);
  }
  return (void *)((char *)a + sizeof(GCAhdr));'''
assert old in s;s=s.replace(old,new,1)
old='''    if (lj_arena_gc2_tokens_empty_acq(a) &&
\tlj_arena_gc2_desc_mapping_clear_acq(a))
      arena_unmap_aligned((void *)a, mapsize);'''
new='''    if (lj_arena_gc2_tokens_empty_acq(a) &&
\tlj_arena_gc2_desc_mapping_clear_acq(a)) {
      free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
      arena_unmap_aligned((void *)a, mapsize);
    }'''
assert old in s;s=s.replace(old,new,1)
old='''  if (p && mapsize)
    arena_unmap_aligned((void *)lj_arena_of(p), mapsize);'''
new='''  if (p && mapsize) {
    GCArena *a = lj_arena_of(p);
    free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
    arena_unmap_aligned((void *)a, mapsize);
  }'''
assert old in s;s=s.replace(old,new,1);f.write_text(s)
# No arena-empty behavior from28de is overwritten by the rebased insertion.
assert '(flags & (LJ_AF_FLAG_MASK & ~LJ_AF_EMPTY_RECLAIMED))' in s
shutil.copytree(t,p/'tail')
f=p/'tail/src/lj_arena.c';s=f.read_text()
old='''  size_t need = size + sizeof(GCAhdr);
  if (size <= LJ_HUGE_THRESHOLD ||
      need < size || need > ~(size_t)LJ_ARENA_MASK)
    return 0;
  return (need + LJ_ARENA_MASK) & ~(size_t)LJ_ARENA_MASK;'''
new='''  const size_t overhead = sizeof(GCAhdr) + sizeof(LJGC2TabWideStamp);
  size_t need;
  /* Size-only physical geometry applies to both immutable mapping kinds.
  ** HugeTab still publishes the exact logical payload size, never padding/W. */
  if (size <= LJ_HUGE_THRESHOLD ||
      size > ~(size_t)LJ_ARENA_MASK - overhead)
    return 0;
  need = size + overhead;
  return (need + LJ_ARENA_MASK) & ~(size_t)LJ_ARENA_MASK;'''
assert old in s;s=s.replace(old,new,1)
old='''  if (flags & LJ_AF_TRAVERSABLE) {
    LJGC2TabWideStamp *wide = NULL;
    if (!arena_test_gc2_sidecar_alloc_fails())
      wide = (LJGC2TabWideStamp *)calloc(1, sizeof(*wide));
    if (!wide) {
      arena_unmap_aligned(a, mapsize);
      return NULL;  /* No mapping, header or body has been published. */
    }
    la_storeptr_rel((void **)&a->hdr.gc2_huge_wide, wide);
  }'''
new='''  if (flags & LJ_AF_TRAVERSABLE) {
    LJGC2TabWideStamp *wide = (LJGC2TabWideStamp *)
      ((char *)a + mapsize - sizeof(LJGC2TabWideStamp));
    /* arena_map_aligned returns fresh anonymous zero-filled storage. This
    ** initializes the whole W before publication without touching its page.
    ** W stays at this physical tail across same-extent logical reallocs.
    ** No recycled/dirty mapping may enter this private initialization path. */
    la_storeptr_rel((void **)&a->hdr.gc2_huge_wide, wide);
  }'''
assert old in s;s=s.replace(old,new,1)
s=s.replace('''      free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
''','',1)
s=s.replace('''    free(la_loadptr_acq((void *const *)&a->hdr.gc2_huge_wide));
''','',1)
f.write_text(s)
f=p/'tail/src/lj_arena.h';s=f.read_text();s=s.replace('huge mappings own one reserved W.','huge mappings own one reserved W in their physical mapping tail.',1)
old='LJ_FUNC size_t lj_arena_huge_mapsize(size_t size);'
new='''/* Physical extent for either Huge kind: header + exact logical payload +
** one aligned16-byte tail reservation, rounded to the arena quantum.
** The plain kind leaves its W pointer NULL. Neither padding nor W belongs
** to HugeTab logical lookup, vector bounds, copying or live-byte accounting. */
LJ_FUNC size_t lj_arena_huge_mapsize(size_t size);'''
assert old in s;s=s.replace(old,new,1);s=s.replace('LJ_STATIC_ASSERT(sizeof(LJGC2TabStamp) == 16u);','LJ_STATIC_ASSERT(sizeof(LJGC2TabWideStamp) == 16u);\nLJ_STATIC_ASSERT(sizeof(LJGC2TabStamp) == 16u);',1);f.write_text(s)
files=['src/lj_arena.c','src/lj_arena.h','src/lj_gc2.c','src/lj_gc2.h'];manifest={'base':base,'parent_patch_sha256':hashlib.sha256(parent.read_bytes()).hexdigest(),'variants':{}}
for variant in ['calloc','tail']:
 patch='';rows=[]
 for name in files:
  old=subprocess.check_output(['git','show',base+':'+name],cwd='/workspaces/lj-lockless').decode();new=(p/variant/name).read_text()
  patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
  rows.append({'path':name,'sha256':hashlib.sha256(new.encode()).hexdigest()})
 (p/(variant+'-integrated.patch')).write_text(patch);manifest['variants'][variant]={'files':rows,'patch_sha256':hashlib.sha256(patch.encode()).hexdigest()}
patch=''
for name in files:
 old=(p/'calloc'/name).read_text();new=(p/'tail'/name).read_text()
 patch+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+name,tofile='b/'+name))
(p/'tail-vs-calloc.patch').write_text(patch)
(p/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(p)
