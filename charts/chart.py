import pandas as pd
import matplotlib.pyplot as plt
df = pd.read_csv("data.csv")
for k,v in df.groupby(["name", "threads"])["ns_per_cycle"].mean().reset_index().groupby("name"):
    v.set_index("threads")["ns_per_cycle"].plot(label=k, style='-o')
plt.yscale('log')
plt.xscale('log')
plt.ylabel('ns per call')
plt.xlabel('threads')
plt.grid()
plt.legend()
plt.savefig("chart.png")
