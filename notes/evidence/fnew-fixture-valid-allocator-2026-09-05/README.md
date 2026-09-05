# Evidence scope

The runtime source is commit 28de50a622e489019fa22845d6454e029b210582 plus
`fnew-fixture-repair.patch`. `setup.json` records the four changed source
hashes. `final-validation.json` records all inspected build inputs, generated
sources, objects, archives and fixture executables. Binary identities refer to
the retained isolated build; this compact package contains text artifacts,
not copies of the binaries or two identical 676,910-byte preprocessor outputs.

`results.json` identifies commands, flags, environment, output files and exit
codes. `validate.py` is the original driver and expects the isolated tree layout
recorded there. To reproduce in a fresh scratch directory, extract the exact
base into `canonical/` and `strict/`, apply the patch to each, create `tmp/`,
and copy the retained control and driver. The driver uses a separate LuaJIT
binary to run the canonical build harness so it does not rebuild its own
running interpreter. Its original bootstrap was the exact d680 strict binary
from the corrected AoS study's *baseline* directory; no wide runtime is linked
into the FNEW repair fixture.

The canonical registration performs its own clean FUNC+TAB helper build. The
strict command lists all helper/assert flags. The normal-preprocessor check
feeds old and repaired lj_func.c source through the same GCC invocation and
include tree with helpers absent; the identical full-output hashes are
retained. It establishes source preprocessing identity, not a claim that
debug paths or complete separately built binaries are byte-identical.

`forged-identity-control.c` is the deliberately invalid constructor sequence
with a check for unfinished ownership. Its patch is relative to the exact
base fixture. SIGABRT is the required negative result; it is not a failed
positive check. The raw original scheduling failures and stalled AoS
constructor diagnosis remain in the separate earlier study, with durable
relative paths and hashes in `earlier-boundary-evidence.json`.

`artifact-manifest.json` hashes every text artifact in this directory and the
top-level note. It is separate from the recorded source/binary manifest, so
readers can distinguish files included here from retained build identities.
