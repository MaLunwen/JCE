/* Optional project name-confidentiality contract; content security is separate. */
#ifndef JCE_DIST_NAME_POLICY_H
#define JCE_DIST_NAME_POLICY_H
#include <string>
#include <vector>
enum { JCE_DIST_POLICY_MANIFEST_CAP = 1024 * 1024,
       JCE_DIST_CONFIDENTIAL_PATH_CAP = 1023,
       JCE_DIST_CONFIDENTIAL_COUNT_CAP = 256,
       JCE_DIST_CONFIDENTIAL_BYTES_CAP = 64 * 1024 };
/* Public host FS/JSON only. Missing key means no confidential names.
 * Returns sorted unique canonical addresses, never an exclusion list.
 * The graph owner also checks exact selected-address membership. */
bool jce_dist_name_policy_load(const std::string &manifest_path,
                               std::vector<std::string> *out_paths,
                               std::string *out_error);
#endif
