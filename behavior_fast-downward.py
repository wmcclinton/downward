import os
import glob

path_to_tasks = "/home/wbm3/Documents/GitHub/bddl/bddl/activity_definitions"

tasks_ran = []
for folder_name in glob.glob(path_to_tasks + "/*"):
    task_name = folder_name.split("/")[-1]
    if "domain_" in task_name:
        continue 
    f = open(f"{path_to_tasks}/{task_name}/problem0.pddl", "r")
    if "stained" in f.read():
        tasks_ran.append([task_name, False])
        continue
    f.close()
    # Run Fast Downward
    result = os.popen(f"./fast-downward.py behavior_full.pddl {path_to_tasks}/{task_name}/problem0.pddl --search \"lazy_greedy([ff()], preferred=[ff()])\"").read()
    if "Solution" in result:
        f = open("sas_plan", "r")
        if "cost = 0" in f.read():
            print(task_name, "fails because of cost = 0 plan")
            tasks_ran.append([task_name, False])
        else:
            tasks_ran.append([task_name, True])
        f.close()
    else:
        tasks_ran.append([task_name, False])

sucesses = [task[0] for task in tasks_ran if task[1]]
failures = [task[0] for task in tasks_ran if not task[1]]
print("Ran on", len(tasks_ran), "tasks")
print("Failed", len(failures))
print("Success", len(sucesses))
print(sucesses)