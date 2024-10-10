#!/usr/bin/python3

import os
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

# Open CSV files. This NEEDS to be changed if you are using a different benchmark!
# TODO: Use command line arguments to choose the benchmark.
data_ralloc = pd.read_csv("./prod-con/prod-con_r.csv")
data_makalu = pd.read_csv("./prod-con/prod-con_mak.csv")
data_pmdk = pd.read_csv("./prod-con/prod-con_pmdk.csv")
