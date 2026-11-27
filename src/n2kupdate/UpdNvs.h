#pragma once
// ============================================================================
// UpdNvs - the NVS records of the update path, namespace "n2kupd".
//
// Anything one firmware image tells another goes through NVS and never
// through RTC_NOINIT variables: their addresses differ between builds
// (docs/N2K-UPDATE.md 10.5). Every record is a fixed little struct stored as
// one blob; a record of the wrong size counts as absent.
//
//   h5      {image id, mode}       written at APPLY: the image that has to
//                                  prove N2K reception before it is marked
//                                  valid (10.2 H5); "Later" switches the mode
//   preweb  {sha, size, id, ver}   a companion web UI approved in the
//                                  firmware dialog (5.3.6)
//   webver  {web UI id, version}   the version of the stored index.html.gz
//                                  (9.4.7)
//   cause   {image id, cause, det} why OtaGuard or the crash-loop guard is
//                                  about to roll an image back (10.2, 10.6)
//   rbinfo  {id, version, cause, det}  the last rollback, as reported (10.5)
//   rbseen  {image id}             the rollback notice already shown
//   clg     {image id, seconds, other id}  crash-loop guard window (10.6);
//                                  `other id` is the image a rollback would
//                                  go back to, as it was at mark-valid
//   dirty   {}                     the other slot is being written: from
//                                  OtaWriter::begin() until the slot is
//                                  activated or erased again. What it holds
//                                  meanwhile is no rolled-back image (10.5).
// ============================================================================
#include <stdint.h>

namespace UpdNvs {

enum H5Mode : uint8_t { H5_PROTOCOL = 0, H5_ANY_FRAME = 1 };

struct H5     { uint8_t imageId[8]; uint8_t mode; };
struct PreWeb { uint8_t sha[32]; uint32_t size; uint8_t imageId[8]; uint8_t version[16]; };
struct WebVer { uint8_t id[8]; uint8_t version[16]; };
struct Cause  { uint8_t imageId[8]; uint8_t cause; uint8_t detail; };
struct RbInfo { uint8_t imageId[8]; uint8_t version[16]; uint8_t cause; uint8_t detail; };
struct Clg    { uint8_t imageId[8]; uint32_t seconds; uint8_t otherId[8]; };

bool loadH5(H5 &r);          bool storeH5(const H5 &r);          void clearH5();
bool loadPreWeb(PreWeb &r);  bool storePreWeb(const PreWeb &r);  void clearPreWeb();
bool loadWebVer(WebVer &r);  bool storeWebVer(const WebVer &r);
bool loadCause(Cause &r);    bool storeCause(const Cause &r);    void clearCause();
bool loadRbInfo(RbInfo &r);  bool storeRbInfo(const RbInfo &r);  void clearRbInfo();
bool loadRbSeen(uint8_t id[8]); bool storeRbSeen(const uint8_t id[8]); void clearRbSeen();
bool loadClg(Clg &r);        bool storeClg(const Clg &r);        void clearClg();
bool slotDirty();            bool setSlotDirty();                void clearSlotDirty();

}  // namespace UpdNvs
