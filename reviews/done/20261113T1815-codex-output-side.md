1. **[P1] Concurrent links can corrupt the same output inode.** [link/write.c:74](/home/exedev/bottom-up/link/write.c:74)  
   Two invocations can both open the old output before either applies mode 0. Both then pass `fstat`, truncate, zero and write through independent mappings. Different-sized outputs can cause SIGBUS; equal-sized outputs can become a mixture while both invocations report success. Mode changes do not revoke already-open descriptors. Reuse needs serialization covering the entire write.

2. **[P1] Existing readers can consume a corrupt executable.** [link/write.c:82](/home/exedev/bottom-up/link/write.c:82)  
   A concurrent installer or cache uploader that opened the old executable before `fchmod` continues reading that inode during truncation and rewriting. It can publish mixed old/new bytes or a truncated binary; an existing mapping can SIGBUS when the file shrinks. `ETXTBSY` protects executing binaries, not readers. This regresses the previous unlink-and-create behavior.

3. **[P1] Lazy joining misses weak references to otherwise-live content.** [link/resolve.c:372](/home/exedev/bottom-up/link/resolve.c:372), [link/dead.c:294](/home/exedev/bottom-up/link/dead.c:294)  
   Consider equal-hash copies A and B: a live symbol weakly references A, while another live symbol strongly references B. The flood skips the weak reference, and `hashedRecords` excludes A because it has no reachability flags. Consequently A remains self-canonical with address zero, even though B is laid out. The writer substitutes `runtime.unreachableMethod` for a weak `R_ADDR` instead of pointing to B. Unreached copies referenced by emitted weak relocations still need lookup against the surviving hash classes.

4. **[P2] Killing a link before writing leaves the stale executable runnable.** [link/main.c:448](/home/exedev/bottom-up/link/main.c:448)  
   Default reuse leaves the previous output untouched throughout loading, resolution, dead-code elimination and layout. A SIGKILL, crash or killed parent during those phases bypasses error cleanup, leaving the previous executable with its original permissions. The “killed link leaves mode 0” guarantee only starts in `reuseOutput`. The kill test uses `-fresh`, so it misses this case.

5. **[P2] The single-hard-link check does not protect links created afterward.** [link/write.c:77](/home/exedev/bottom-up/link/write.c:77)  
   A backup process can hard-link the output after `fstat` observes `st_nlink == 1`, but before truncation. That supposedly preserved backup then receives the new bytes and permission changes too. Neither mode 0 nor the open writable descriptor prevents the owner from creating the link. The existing hard-link test covers only links present before reuse begins.

6. **[P2] Completion is announced before the child releases stdout/stderr.** [link/main.c:397](/home/exedev/bottom-up/link/main.c:397)  
   If the child is descheduled or stopped immediately after sending `K`, the parent exits successfully while the child still owns the caller’s pipe descriptors. A caller collecting output until EOF remains blocked, potentially indefinitely for a stopped child. Redirect or close standard streams before sending the completion byte.

Static review only; no files changed or tests run. I found no additional concrete defect in the 2 MiB segment boundaries, file-backed BSS, or runtime/moduledata boundaries.