#ifndef __CFG80211_DEBUGFS_H
#define __CFG80211_DEBUGFS_H

#ifdef CONFIG_CFG80211_DEBUGFS
void cfg80211_debugfs_rdev_add(struct cfg80211_registered_device *rdev);
void ieee80211_debugfs_add_glbl(struct dentry *dir);
#else
static inline
void cfg80211_debugfs_rdev_add(struct cfg80211_registered_device *rdev) {}
static inline void ieee80211_debugfs_add_glbl(struct dentry *dir) { }
#endif

#endif /* __CFG80211_DEBUGFS_H */
