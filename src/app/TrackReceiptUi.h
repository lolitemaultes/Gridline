// Gridline - J-219: Track Receipt (Help > Track Receipt...). Which parts of
// which original recordings the project uses, to help fill in a sample
// license (for example on Tracklib). Three pages: the picker (every audio file
// the project uses, grouped by song, licensed samples ticked), the measurement
// (an offline pass over the project on a background thread, with progress and
// Cancel) and the results (seconds per original song, tier, the parts used and
// where they come from, the fields to fill in). "Save Track Receipt" writes
// "<project>_trackreceipt.xml" (a spreadsheet); "View results" writes the PDF
// and opens it. The typed fields are saved in the project (Song::trackReceipt).
#pragma once

namespace gl {

class App;

namespace receiptui {

void open(App& a);
// Every frame (from drawDialogs).
void draw(App& a);
// App exit: a running measurement is cancelled and joined.
void shutdown();

}  // namespace receiptui

}  // namespace gl
