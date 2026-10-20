// Gridline — FL Studio project export (J-34): event writer, sections, plugin
// encoders and corpus round trips. Corpus checks read FL Studio's shipped
// projects and presets when FL is installed and are skipped otherwise.
#include "flp_export/core.h"
#include "flp_export/import_fixes.h"
#include "flp_export/enc_FxEq.h"
#include "flp_export/enc_FxDyn.h"
#include "flp_export/enc_FxTime.h"
#include "flp_export/enc_FxK3.h"
#include "flp_export/enc_FxMisc.h"
#include "flp_export/enc_Fx2Mod.h"
#include "flp_export/enc_Fx2Pitch.h"
#include "flp_export/enc_Fx2Space.h"
#include "flp_export/enc_Fx2Tone.h"
#include "flp_export/enc_Dec3.h"
#include "flp_export/enc_GenA.h"
#include "flp_export/enc_GenB.h"
#include "flp_export/enc_J22.h"
#include "flp_export/enc_Wrapper.h"
#include "flp_export/enc_J39.h"
#include "flp_export/j38_fades.h"

int main() {
  testCore();
  testImportFixes();
  enctest::testEncFxEq();
  enctest::testEncFxDyn();
  enctest::testEncFxTime();
  enctest::testEncFxK3();
  enctest::testEncFxMisc();
  enctest::testEncFx2Mod();
  enctest::testEncFx2Pitch();
  enctest::testEncFx2Space();
  enctest::testEncFx2Tone();
  enctest::testEncDec3();
  enctest::testEncGenA();
  enctest::testEncGenB();
  enctest::testEncJ22();
  enctest::testEncWrapper();
  enctest::testEncJ39();
  enctest::testExportJ39();
  testJ38Fades();
  return finish();
}
