# Project Status

- Active branch: `add-virtual-folders-to-nav-top`
- Working file: `mod.wh.cpp`
- Current local scope: unload safety on disable, separator color sampling restored, divider height at `1px`, post-startup `[INSERT-TRACE]` logging for unexpected depth-1 inserts, post-startup `[SEL-TRACE]` logging for depth-1 selection changes during drag/drop investigation, and separator redraw respecting both `Remove separator below top nav` and `Remove separator below Quick Access`
- Code style: no forward refs; move small helpers to the dependency site instead
- Separator rule: collapsing spacer height and drawing the separator are separate decisions; never draw a managed separator when its corresponding `Remove separator ...` toggle is on
- Build preference: do not compile from here; use the Windhawk UI build button when a build is needed
- Push note: if this branch history is rewritten again, use `git push --force-with-lease origin add-virtual-folders-to-nav-top` only when explicitly asked
