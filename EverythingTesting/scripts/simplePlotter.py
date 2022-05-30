import matplotlib.pyplot as plt

throughput_index = 6
time_index = 4
request_index = 5

class ItemToPlot:
    def __init__(self, label,getDataFunction,args):
        self.label = label
        self.getDataFunction = getDataFunction
        self.args = args
        
    def getData(self):
        return self.getDataFunction(*self.args)
        
        
def take_average(file,index):
    file1 = open(file, 'r')
    throughput = 0
    counter = 0
    while True:
        line = file1.readline()
        if not line:
            break
        tab = line.split(" ")
        throughput += float(tab[index])
        counter +=1
    return(throughput/counter)

def get_full_data(file,index):
    file1 = open(file, 'r')
    data = []
    while True:
        line = file1.readline()
        if not line:
            break
        tab = line.split(" ")
        data.append(float(tab[index]))
    return data


def comparison_plot_bar(items,title,yLabel,outputFileName):
    data = [i.getData() for i in items]
    labels = [i.label for i in items]
    plt.title(title)
    plt.ylabel(yLabel)
    plt.bar(labels,data)
    plt.savefig(outputFileName)
    
def comparison_plot_box(items,title,yLabel,outputFileName):
    data = [i.getData() for i in items]
    labels = [i.label for i in items]
    fig, ax = plt.subplots()
    ax.boxplot(data,showfliers=False)
    ax.set_xticklabels(labels)
    ax.set_title(title)
    ax.set_ylabel(yLabel)
    plt.savefig(outputFileName)

    
def throughput_comparison_plot_bar():
    
    item1 = ItemToPlot("nodpdk",take_average,("../data/output_nodpdk_tp_enc.txt",throughput_index))
    item2 = ItemToPlot("dpdk",take_average,("../data/output_dpdk_tp_enc.txt",throughput_index))
    comparison_plot_bar([item1,item2],"Throughput comparison","Throughput(Mbps)","../plots/Throughput_bar.png")
    
    
def throughput_comparison_plot_box():
    item1 = ItemToPlot("nodpdk",get_full_data,("../data/output_nodpdk_tp_enc.txt",throughput_index))
    item2 = ItemToPlot("dpdk",get_full_data,("../data/output_dpdk_tp_enc.txt",throughput_index))
    comparison_plot_box([item1,item2],"Throughput comparison","Throughput(Mbps)","../plots/Throughput_box.png")
    
    
    
def handshake_comparison_plot():
    def dataFunction(file,index):
        data = get_full_data(file,index)
        return [d/20 for d in data]
    item1 = ItemToPlot("nodpdk",dataFunction,("../data/handshake_nodpdk.txt",request_index))
    item2 = ItemToPlot("dpdk",dataFunction,("../data/handshake_dpdk.txt",request_index))
    comparison_plot_box([item1,item2],"Handshake performance","Number of handshake completed (hz)","../plots/HandshakeComparison.png")
    
    
def server_scaling_plot():
    items = []
    for i in range(1,16):
        item = ItemToPlot(str(i),get_full_data,("../data/server_scaling_dpdk_{}.txt".format(str(i)),throughput_index))
        items.append(item)
    comparison_plot_box(items,"RSS analysis","individual throughput (Mbps)","../plots/server_scaling.png")
    
def proxy_pkt_size_plot():
    items = []
    for i in [10,100,1000]:
        item = ItemToPlot("payload_size : {}".format(str(i)),get_full_data,("../data/proxy_{}.txt".format(str(i)),3))
        items.append(item)
    comparison_plot_box(items, "Packet size impact","time elpased (s)","../plots/proxy_pkt_size.png")

def noproxy_pkt_size_plot():
    items = []
    for i in [10,100,1000]:
        item = ItemToPlot("payload_size : {}".format(str(i)),get_full_data,("../data/noproxy_{}.txt".format(str(i)),3))
        items.append(item)
    comparison_plot_box(items, "Packet size impact without proxy","time elpased (s)","../plots/noproxy_pkt_size.png")
    
def batching_no_CC_plot():
    items = []
    for batching in [4,8,16,32,64,128]:
        item = ItemToPlot("{}".format(str(batching)),get_full_data,("../data/throughput_noCC_noPacing_{}_dpdk.txt".format(str(batching)),throughput_index))
        items.append(item)
    comparison_plot_box(items, "Batching size impact on throughput","Throughput (Mbps)","../plots/batching_impact_noCC.png")

def batching_plot():
    items = []
    for batching in [4,8,16,32,64]:
        item = ItemToPlot("{}".format(str(batching)),get_full_data,("../data/throughput_{}_dpdk.txt".format(str(batching)),throughput_index))
        items.append(item)
    comparison_plot_box(items, "Batching size impact on throughput","Throughput (Mbps)","../plots/batching_impact_withCC.png")
    
if __name__ == "__main__":
    #handshake_comparison_plot()
    #throughput_comparison_plot_box()
    #server_scaling_plot()
    #noproxy_pkt_size_plot()
    batching_no_CC_plot()
    batching_plot()
    
    
   

