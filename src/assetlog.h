#ifndef WF_ASSETLOG_H
#define WF_ASSETLOG_H

/* --debug / WF_DEBUG=1: print every external asset path. */
extern int wf_debug_assets;

void wf_asset_log(const char *kind, const char *path);
void wf_debug_assets_from_env(void);

#endif
