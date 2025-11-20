#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- Configuration ---
#define MAX_LINE 8192
#define MAX_PATH 1024

// HARDCODED PATHS AND FILENAMES
#define BASE_DIR "/Users/anuragpadhy/Desktop/SSASS2/"
#define INPUT_UPA "UPA.txt"
#define INPUT_TIME "TIME.txt"

#define OUTPUT_UA "UA_TIME.txt"
#define OUTPUT_PA "PA_TIME.txt"
#define OUTPUT_REB "REB_TIME.txt"

// --- Structures ---

typedef struct {
    double start;
    double end;
} Interval;

typedef struct {
    int uid;
    int pid;
    Interval t;
    int id;         // Original index to track specific assignment
    int covered;    // 1 if covered, 0 if not
} TupaEntry;

typedef struct {
    int *uids;
    int num_uids;
    int *pids;
    int num_pids;
    Interval t;
    int id;
} Role;

typedef struct {
    TupaEntry *entries;
    int count;
    int capacity;
    int num_users;
    int num_perms;
    // Inverted Index for O(1) lookup speedup
    int **user_map; 
    int *user_map_counts;
} Dataset;

// --- Helper Functions ---

double max_d(double a, double b) { return (a > b) ? a : b; }
double min_d(double a, double b) { return (a < b) ? a : b; }

double duration(Interval iv) {
    return max_d(0.0, iv.end - iv.start);
}

// Jaccard Similarity (Intersection / Union)
double calculate_sigma(Interval t1, Interval t2) {
    double inter_start = max_d(t1.start, t2.start);
    double inter_end = min_d(t1.end, t2.end);
    
    if (inter_start >= inter_end) return 0.0; // Disjoint
    
    double union_start = min_d(t1.start, t2.start);
    double union_end = max_d(t1.end, t2.end);
    
    double inter_len = inter_end - inter_start;
    double union_len = union_end - union_start;
    
    if (union_len <= 1e-9) return 0.0;
    return inter_len / union_len;
}

void add_to_array(int **arr, int *count, int val) {
    *arr = realloc(*arr, (*count + 1) * sizeof(int));
    (*arr)[*count] = val;
    (*count)++;
}

// --- Data Loading ---

void build_index(Dataset *ds) {
    ds->user_map = malloc((ds->num_users + 2) * sizeof(int*));
    ds->user_map_counts = calloc((ds->num_users + 2), sizeof(int));
    
    for(int i = 0; i <= ds->num_users; i++) ds->user_map[i] = NULL;

    for(int i = 0; i < ds->count; i++) {
        int u = ds->entries[i].uid;
        // Safety check for bounds
        if(u <= ds->num_users) {
            add_to_array(&ds->user_map[u], &ds->user_map_counts[u], i);
        }
    }
}

Dataset load_data(const char *upa_path, const char *time_path) {
    Dataset ds;
    ds.count = 0;
    ds.capacity = 1000;
    ds.entries = malloc(ds.capacity * sizeof(TupaEntry));

    printf("Loading UPA from: %s\n", upa_path);
    printf("Loading TIME from: %s\n", time_path);

    FILE *f_upa = fopen(upa_path, "r");
    FILE *f_time = fopen(time_path, "r");

    if (!f_upa || !f_time) {
        printf("Error: Could not open input files.\nMake sure '%s' and '%s' exist in '%s'.\n", INPUT_UPA, INPUT_TIME, BASE_DIR);
        exit(1);
    }

    char line_upa[MAX_LINE];
    char line_time[MAX_LINE];

    // Read Headers from UPA
    // Line 1: Users, Line 2: Perms, Line 3: Total Entries
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_users = atoi(line_upa);
    if (fgets(line_upa, MAX_LINE, f_upa)) ds.num_perms = atoi(line_upa);
    if (fgets(line_upa, MAX_LINE, f_upa)) { /* skip total lines count */ }

    int id_counter = 0;

    // Read UPA and TIME in parallel
    while (fgets(line_upa, MAX_LINE, f_upa) && fgets(line_time, MAX_LINE, f_time)) {
        int uid, pid;
        if (sscanf(line_upa, "%d %d", &uid, &pid) != 2) continue;

        // Parse TIME line (handles tabs/spaces and multiple intervals)
        char *ptr = line_time;
        double s, e;
        int offset;
        
        // Keep reading pairs of doubles from the line
        while (sscanf(ptr, "%lf %lf%n", &s, &e, &offset) == 2) {
            if (ds.count >= ds.capacity) {
                ds.capacity *= 2;
                ds.entries = realloc(ds.entries, ds.capacity * sizeof(TupaEntry));
            }
            ds.entries[ds.count].uid = uid;
            ds.entries[ds.count].pid = pid;
            ds.entries[ds.count].t.start = s;
            ds.entries[ds.count].t.end = e;
            ds.entries[ds.count].id = id_counter++;
            ds.entries[ds.count].covered = 0;
            ds.count++;
            
            ptr += offset;
        }
    }

    fclose(f_upa);
    fclose(f_time);
    
    build_index(&ds);
    return ds;
}

// --- TRMP-MVCL Algorithm ---

void solve_trmp(Dataset *ds, double theta, Role **roles_out, int *num_roles_out) {
    int num_roles = 0;
    Role *roles = NULL;
    int remaining_entries = ds->count;
    int *checked_users = calloc(ds->num_users + 2, sizeof(int));

    // Main Greedy Loop
    while (remaining_entries > 0) {
        double max_coverage = -1.0;
        Role best_role = {0};
        int found = 0;

        memset(checked_users, 0, (ds->num_users + 2) * sizeof(int));

        // Iterate through potential seeds
        for (int i = 0; i < ds->count; i++) {
            if (ds->entries[i].covered) continue;

            int uid = ds->entries[i].uid;
            // Optimization: Only check one seed per user per pass
            if (checked_users[uid]) continue;
            checked_users[uid] = 1;

            Interval seed_t = ds->entries[i].t;

            // 1. Identify Candidate Permissions (P_l) for this user
            int *cand_pids = NULL; 
            int num_cp = 0;
            
            int *u_idx = ds->user_map[uid];
            int u_cnt = ds->user_map_counts[uid];

            for(int k=0; k<u_cnt; k++) {
                TupaEntry *e = &ds->entries[u_idx[k]];
                if (!e->covered && calculate_sigma(e->t, seed_t) >= theta) {
                    int exists = 0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == e->pid) { exists=1; break; }
                    if(!exists) add_to_array(&cand_pids, &num_cp, e->pid);
                }
            }

            if(num_cp == 0) { free(cand_pids); continue; }

            // 2. Identify Candidate Users (U_l)
            int *cand_uids = NULL;
            int num_cu = 0;

            for (int u = 1; u <= ds->num_users; u++) {
                if (ds->user_map_counts[u] < num_cp) continue;

                int has_all = 1;
                for (int p_idx = 0; p_idx < num_cp; p_idx++) {
                    int needed_p = cand_pids[p_idx];
                    int p_ok = 0;
                    
                    int *chk_idx = ds->user_map[u];
                    int chk_cnt = ds->user_map_counts[u];

                    for(int c=0; c<chk_cnt; c++) {
                        TupaEntry *e = &ds->entries[chk_idx[c]];
                        if(e->pid == needed_p && !e->covered) {
                            if (calculate_sigma(e->t, seed_t) >= theta) {
                                p_ok = 1; break;
                            }
                        }
                    }
                    if (!p_ok) { has_all = 0; break; }
                }
                if (has_all) add_to_array(&cand_uids, &num_cu, u);
            }

            // 3. Calculate Coverage
            double current_cov = 0.0;
            for(int u_i=0; u_i<num_cu; u_i++) {
                int u = cand_uids[u_i];
                int *chk_idx = ds->user_map[u];
                int chk_cnt = ds->user_map_counts[u];
                
                for(int c=0; c<chk_cnt; c++) {
                    TupaEntry *e = &ds->entries[chk_idx[c]];
                    if (e->covered) continue;

                    int is_in_perm = 0;
                    for(int p=0; p<num_cp; p++) if(cand_pids[p] == e->pid) { is_in_perm=1; break; }
                    
                    if(is_in_perm && calculate_sigma(e->t, seed_t) >= theta) {
                        current_cov += duration(e->t);
                    }
                }
            }

            if (current_cov > max_coverage) {
                if (found) { free(best_role.uids); free(best_role.pids); }
                best_role.uids = cand_uids;
                best_role.num_uids = num_cu;
                best_role.pids = cand_pids;
                best_role.num_pids = num_cp;
                best_role.t = seed_t;
                max_coverage = current_cov;
                found = 1;
            } else {
                free(cand_uids);
                free(cand_pids);
            }
        }

        if (found) {
            best_role.id = ++num_roles;
            roles = realloc(roles, num_roles * sizeof(Role));
            roles[num_roles-1] = best_role;

            for(int u_i=0; u_i<best_role.num_uids; u_i++) {
                int u = best_role.uids[u_i];
                int *chk_idx = ds->user_map[u];
                int chk_cnt = ds->user_map_counts[u];

                for(int c=0; c<chk_cnt; c++) {
                    TupaEntry *e = &ds->entries[chk_idx[c]];
                    if (e->covered) continue;

                    int is_in_perm = 0;
                    for(int p=0; p<best_role.num_pids; p++) if(best_role.pids[p] == e->pid) { is_in_perm=1; break; }

                    if(is_in_perm && calculate_sigma(e->t, best_role.t) >= theta) {
                        e->covered = 1;
                        remaining_entries--;
                    }
                }
            }
        } else {
            break;
        }
    }
    free(checked_users);
    *roles_out = roles;
    *num_roles_out = num_roles;
}

// --- Output Generation ---

void write_outputs(Dataset *ds, Role *roles, int num_roles) {
    char f_ua[MAX_PATH], f_pa[MAX_PATH], f_reb[MAX_PATH];
    
    // Construct absolute output paths
    sprintf(f_ua, "%s%s", BASE_DIR, OUTPUT_UA);
    sprintf(f_pa, "%s%s", BASE_DIR, OUTPUT_PA);
    sprintf(f_reb, "%s%s", BASE_DIR, OUTPUT_REB);

    printf("Writing outputs to:\n  %s\n  %s\n  %s\n", f_ua, f_pa, f_reb);

    // Write UA
    FILE *ua = fopen(f_ua, "w");
    if(!ua) { printf("Error writing UA file.\n"); return; }
    fprintf(ua, "%d\n%d\n", ds->num_users, num_roles);
    for(int i=0; i<num_roles; i++) {
        for(int j=0; j<roles[i].num_uids; j++) {
            fprintf(ua, "%d %d\n", roles[i].uids[j], roles[i].id);
        }
    }
    fclose(ua);

    // Write PA
    FILE *pa = fopen(f_pa, "w");
    if(!pa) { printf("Error writing PA file.\n"); return; }
    fprintf(pa, "%d\n%d\n", num_roles, ds->num_perms);
    for(int i=0; i<num_roles; i++) {
        for(int j=0; j<roles[i].num_pids; j++) {
            fprintf(pa, "%d %d\n", roles[i].pids[j], roles[i].id);
        }
    }
    fclose(pa);

    // Write REB
    FILE *reb = fopen(f_reb, "w");
    if(!reb) { printf("Error writing REB file.\n"); return; }
    fprintf(reb, "%d\n", num_roles);
    for(int i=0; i<num_roles; i++) {
        fprintf(reb, "%d %.1f %.1f\n", roles[i].id, roles[i].t.start, roles[i].t.end);
    }
    fclose(reb);
    
    // Console Output Metrics
    int pa_count = 0;
    int reb_count = num_roles; 
    
    for(int i=0; i<num_roles; i++) {
        pa_count += roles[i].num_pids;
    }
    
    printf("|R| = %d\n", num_roles);
    printf("|PA| + |REB| = %d\n", pa_count + reb_count);
}

// --- Main ---

int main(int argc, char *argv[]) {
    // Only Theta is required now
    if (argc < 2) {
        printf("Usage: %s <THETA>\n", argv[0]);
        printf("Files will be read from: %s\n", BASE_DIR);
        return 1;
    }

    double theta = atof(argv[1]);

    // Construct Full File Paths
    char upa_full_path[MAX_PATH];
    char time_full_path[MAX_PATH];

    sprintf(upa_full_path, "%s%s", BASE_DIR, INPUT_UPA);
    sprintf(time_full_path, "%s%s", BASE_DIR, INPUT_TIME);

    Dataset ds = load_data(upa_full_path, time_full_path);
    
    Role *roles;
    int num_roles;
    
    solve_trmp(&ds, theta, &roles, &num_roles);
    
    write_outputs(&ds, roles, num_roles);
    
    // Minimal cleanup
    free(ds.entries);
    free(ds.user_map);
    for(int i=0; i<num_roles; i++) {
        free(roles[i].uids);
        free(roles[i].pids);
    }
    free(roles);
    
    return 0;
}
