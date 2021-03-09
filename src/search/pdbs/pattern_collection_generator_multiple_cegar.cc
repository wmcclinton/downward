#include "pattern_collection_generator_multiple_cegar.h"

#include "cegar.h"
#include "pattern_database.h"
#include "utils.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/countdown_timer.h"
#include "../utils/markup.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include <vector>

using namespace std;

namespace pdbs {
PatternCollectionGeneratorMultipleCegar::PatternCollectionGeneratorMultipleCegar(
    options::Options &opts)
    : max_refinements(opts.get<int>("max_refinements")),
      max_pdb_size(opts.get<int>("max_pdb_size")),
      max_collection_size(opts.get<int>("max_collection_size")),
      wildcard_plans(opts.get<bool>("wildcard_plans")),
      cegar_max_time(opts.get<double>("max_time")),
      verbosity(opts.get<utils::Verbosity>("verbosity")),
      rng(utils::parse_rng_from_options(opts)),
      random_seed(opts.get<int>("random_seed")),
      stagnation_limit(opts.get<double>("stagnation_limit")),
      blacklist_trigger_percentage(opts.get<double>("blacklist_trigger_percentage")),
      blacklist_on_stagnation(opts.get<bool>("blacklist_on_stagnation")),
      total_max_time(opts.get<double>("total_max_time")) {
}

PatternCollectionInformation PatternCollectionGeneratorMultipleCegar::generate(
    const shared_ptr<AbstractTask> &task) {
    if (verbosity >= utils::Verbosity::NORMAL) {
        utils::g_log << "Generating patterns using the multiple CEGAR algorithm" << endl;
    }

    TaskProxy task_proxy(*task);
    utils::CountdownTimer timer(total_max_time);

    // Store the set of goals in random order.
    vector<FactPair> goals = get_goals_in_random_order(task_proxy, rng);

    // Store the non-goal variables for potential blacklisting.
    int num_vars = task_proxy.get_variables().size();
    vector<int> non_goal_variables;
    non_goal_variables.reserve(num_vars - goals.size());
    for (int var_id = 0; var_id < num_vars; ++var_id) {
        bool is_goal_var = false;
        for (const FactPair &goal : goals) {
            if (var_id == goal.var) {
                is_goal_var = true;
                break;
            }
        }
        if (!is_goal_var) {
            non_goal_variables.push_back(var_id);
        }
    }

    if (verbosity >= utils::Verbosity::DEBUG) {
        utils::g_log << "goal variables: ";
        for (auto goal : goals) {
            utils::g_log << goal.var << ", ";
        }
        utils::g_log << endl;
        utils::g_log << "non-goal variables: " << non_goal_variables << endl;
    }

    // Collect all unique patterns and their PDBs.
    utils::HashSet<Pattern> generated_patterns;
    shared_ptr<PDBCollection> generated_pdbs = make_shared<PDBCollection>();

    bool blacklisting = false;
    double stagnation_start_time = -1;
    int num_iterations = 1;
    int goal_index = 0;
    int remaining_collection_size = max_collection_size;
    const utils::Verbosity cegar_verbosity(utils::Verbosity::SILENT);
    /*
      Start blacklisting after the percentage of total_max_time specified via
      blacklisting_trigger_percentage has passed. Compute this time point once.
    */
    double blacklisting_start_time = total_max_time * blacklist_trigger_percentage;
    while (true) {
        // Check if we need to start blacklisting.
        if (!blacklisting && timer.get_elapsed_time() > blacklisting_start_time) {
            blacklisting = true;
            // Also reset stagnation timer in case it was already set.
            stagnation_start_time = -1;
            if (verbosity >= utils::Verbosity::NORMAL) {
                utils::g_log << "given percentage of total time limit "
                             << "exhausted; enabling blacklisting." << endl;
            }
        }

        unordered_set<int> blacklisted_variables;
        if (blacklisting && !non_goal_variables.empty()) {
            /*
              Randomize the number of non-goal variables for blacklisting.
              We want to choose at least 1 non-goal variable and up to the
              entire set of non-goal variables.
            */
            int blacklist_size = (*rng)(non_goal_variables.size());
            ++blacklist_size; // [1, |non-goal variables|]
            rng->shuffle(non_goal_variables);
            for (int i = 0; i < blacklist_size; ++i) {
                int var_id = non_goal_variables[i];
                blacklisted_variables.insert(var_id);
            }
            if (verbosity >= utils::Verbosity::DEBUG) {
                utils::g_log << "blacklisting " << blacklist_size << " out of "
                             << non_goal_variables.size()
                             << " non-goal variables: ";
                for (int var : blacklisted_variables) {
                    utils::g_log << var << ", ";
                }
                utils::g_log << endl;
            }
        }

        int remaining_pdb_size_for_cegar = min(remaining_collection_size, max_pdb_size);
        double remaining_time_for_cegar = min(timer.get_remaining_time().operator double(), cegar_max_time);
        /*
          Call CEGAR with the remaining size budget (limiting one of pdb and
          collection size would be enough, but this is cleaner), with the
          remaining time limit and an RNG instance with a different random
          seed in each iteration.
        */
        PatternCollectionInformation collection_info = CEGAR(
            max_refinements,
            remaining_pdb_size_for_cegar,
            remaining_collection_size,
            wildcard_plans,
            remaining_time_for_cegar,
            cegar_verbosity,
            make_shared<utils::RandomNumberGenerator>(
                random_seed + num_iterations),
            task,
            {goals[goal_index]},
            move(blacklisted_variables)).compute_pattern_collection();
        shared_ptr<PatternCollection> new_patterns = collection_info.get_patterns();
        shared_ptr<PDBCollection> new_pdbs = collection_info.get_pdbs();
        if (new_patterns->size() > 1) {
            cerr << "a generator computed more than one pattern" << endl;
            utils::exit_with(utils::ExitCode::SEARCH_CRITICAL_ERROR);
        }

        const Pattern &pattern = new_patterns->front();
        if (verbosity >= utils::Verbosity::DEBUG) {
            utils::g_log << "generated patterns " << pattern << endl;
        }
        if (generated_patterns.insert(pattern).second) {
            // CEGAR generated a new pattern. Reset stagnation_start_time.
            stagnation_start_time = -1;

            shared_ptr<PatternDatabase> &pdb = new_pdbs->front();
            remaining_collection_size -= pdb->get_size();
            generated_pdbs->push_back(pdb);

            if (remaining_collection_size <= 0) {
                /*
                  This value can become negative if the given size limits for
                  pdb or collection size are so low that even the singleton
                  goal pattern generated by CEGAR violates it.
                */
                if (verbosity >= utils::Verbosity::NORMAL) {
                    utils::g_log << "collection size limit reached" << endl;
                }
                break;
            }
        } else {
            // Pattern is not new. Set stagnation start time if not already set.
            if (stagnation_start_time == -1) {
                stagnation_start_time = timer.get_elapsed_time();
            }
        }

        if (timer.is_expired()) {
            if (verbosity >= utils::Verbosity::NORMAL) {
                utils::g_log << "time limit reached" << endl;
            }
            break;
        }

        // Test if no new pattern was generated for longer than stagnation_limit.
        if (stagnation_start_time != -1 &&
            timer.get_elapsed_time() - stagnation_start_time > stagnation_limit) {
            if (blacklist_on_stagnation) {
                if (blacklisting) {
                    if (verbosity >= utils::Verbosity::NORMAL) {
                        utils::g_log << "stagnation limit reached "
                                     << "despite blacklisting, terminating"
                                     << endl;
                    }
                    break;
                } else {
                    if (verbosity >= utils::Verbosity::NORMAL) {
                        utils::g_log << "stagnation limit reached, "
                                     << "enabling blacklisting" << endl;
                    }
                    blacklisting = true;
                    stagnation_start_time = -1;
                }
            } else {
                if (verbosity >= utils::Verbosity::NORMAL) {
                    utils::g_log << "stagnation limit reached, terminating" << endl;
                }
                break;
            }
        }

        ++num_iterations;
        ++goal_index;
        goal_index = goal_index % goals.size();
        assert(utils::in_bounds(goal_index, goals));
    }

    shared_ptr<PatternCollection> patterns = make_shared<PatternCollection>();
    patterns->reserve(generated_pdbs->size());
    for (const shared_ptr<PatternDatabase> &pdb : *generated_pdbs) {
        patterns->push_back(pdb->get_pattern());
    }
    PatternCollectionInformation result(task_proxy, patterns);
    result.set_pdbs(generated_pdbs);

    if (verbosity >= utils::Verbosity::NORMAL) {
        utils::g_log << "Multiple CEGAR number of iterations: "
                     << num_iterations << endl;
        utils::g_log << "Multiple CEGAR average time per generator: "
                     << timer.get_elapsed_time() / num_iterations
                     << endl;
        dump_pattern_collection_generation_statistics(
            "Multiple CEGAR",
            timer.get_elapsed_time(),
            result,
            max_collection_size - remaining_collection_size);
    }
    return result;
}

static shared_ptr<PatternCollectionGenerator> _parse(options::OptionParser &parser) {
    parser.document_synopsis(
        "Multiple CEGAR",
        "This pattern collection generator implements the multiple CEGAR algorithm "
        "described in the paper" + utils::format_conference_reference(
            {"Alexander Rovner", "Silvan Sievers", "Malte Helmert"},
            "Counterexample-Guided Abstraction Refinement for Pattern Selection "
            "in Optimal Classical Planning",
            "https://ai.dmi.unibas.ch/papers/rovner-et-al-icaps2019.pdf",
            "Proceedings of the 29th International Conference on Automated "
            "Planning and Scheduling (ICAPS 2019)",
            "362-367",
            "AAAI Press",
            "2019"));
    parser.add_option<double>(
        "total_max_time",
        "maximum time in seconds for the multiple CEGAR algorithm. The "
        "algorithm will always execute at least one iteration, i.e., call the "
        "CEGAR algorithm once. This limit possibly overrides the limit "
        "specified for the CEGAR algorithm.",
        "100.0",
        Bounds("0.0", "infinity"));
    parser.add_option<double>(
        "stagnation_limit",
        "maximum time in seconds the multiple CEGAR algorithm allows without "
        "generating a new pattern through the CEGAR algorithm. The multiple "
        "CEGAR algorithm terminates prematurely if this limit is hit unless "
        "blacklist_on_stagnation is enabled.",
        "20.0",
        Bounds("1.0", "infinity"));
    parser.add_option<double>(
        "blacklist_trigger_percentage",
        "percentage of total_max_time after which the multiple CEGAR "
        "algorithm enables blacklisting for diversification",
        "0.75",
        Bounds("0.0", "1.0"));
    parser.add_option<bool>(
        "blacklist_on_stagnation",
        "If true, the multiple CEGAR algorithm will enable blacklisting for "
        "diversification when stagnation_limit is hit for the first time "
        "(unless it was already enabled due to blacklist_trigger_percentage) "
        "and terminate when stagnation_limit is hit for the second time.",
        "true");
    add_cegar_options_to_parser(parser);
    utils::add_verbosity_option_to_parser(parser);
    utils::add_rng_options(parser);

    Options opts = parser.parse();
    if (parser.dry_run()) {
        return nullptr;
    }

    return make_shared<PatternCollectionGeneratorMultipleCegar>(opts);
}

static Plugin<PatternCollectionGenerator> _plugin("multiple_cegar", _parse);
}
