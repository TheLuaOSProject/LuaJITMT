# ARM64 pure Lua leaf: empty comparison arm

This checkpoint admits a bounded single-comparison leaf whose second arm is
empty and whose paths meet immediately at the common tail. The source decoder
and independent semantic/post-RA replay now allow the join at the second-arm
boundary. At a join, post-RA type propagation treats INT and NUM as a numeric
compatible type for replay through the common tail; snapshot references and
saved values remain exact on each path.

The return fixture covers `if a < 0 then a = a + .5 end; return a + .25`.
It selects the common-tail child snapshot, proves the unchanged copied
semantic and post-RA views accept, and rejects independently mutated compare
operands, cold targets, guard polarity, omitted live entries, wrong mapped
locals, wrong mapped IR values, and a numeric-to-nil comparison source. The
join checks are counted separately from terminal maps.

Validation used
`tools/ci/arm64_jit_lua_leaf_contract.sh` on the frozen source with AppleClang
for arm64 and arm64e, assertion and release profiles. Each profile reported
98 cases, 96 terminal exits, 96 terminal maps, one join map, eight join
semantic rejections, and eight join post-RA rejections. The ordinary matched
probe is `/Users/frityet/Scratch/ljmt-empty-branch-ordinary-arm-final.log`;
the selected ARM root publishes and takes exits for the empty-arm case in both
numeric phases. The corresponding x64 witness is in
`/Users/frityet/Scratch/ljmt-empty-branch-x86_64.tsv` for the same probe
source, `probe.lua`, from the expanded parity inventory. The ordinary probe
also confirms sequential two-condition leaves remain unadmitted on ARM; this
checkpoint does not generalize the CFG proof to multiple decisions.

The earlier four-profile result before join-clone coverage was activated is
superseded by `/Users/frityet/Scratch/ljmt-empty-branch-join-final.log`.
