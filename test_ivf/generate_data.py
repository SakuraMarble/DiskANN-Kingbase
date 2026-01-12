import numpy as np
import struct
import random
import argparse
import os

def generate_data(num_vecs, dim, vec_path, label_path, universal_tag=11):
    print(f"Generating {num_vecs} vectors with dimension {dim}...")
    
    # 1. 生成随机向量 (float32)
    vectors = np.random.rand(num_vecs, dim).astype(np.float32)
    
    # 2. 写入二进制文件
    # 格式: [uint32 num] [uint32 dim] [float data...]
    with open(vec_path, "wb") as f:
        f.write(struct.pack('I', num_vecs))
        f.write(struct.pack('I', dim))
        f.write(vectors.tobytes())
    print(f"Saved vectors to {vec_path}")

    # 3. 生成随机标签
    # 模拟：有的向量标签很多，有的很少，偶尔出现通用标签
    print(f"Generating labels...")
    with open(label_path, "w") as f:
        for i in range(num_vecs):
            # 1% 的概率是通用向量
            if random.random() < 0.01:
                tags = [universal_tag]
            else:
                # 随机生成 1~5 个标签，标签ID范围 0~100
                # 避开通用标签ID，以免逻辑混淆
                num_tags = random.randint(0, 5)
                tags = []
                if num_tags > 0:
                    candidates = list(range(100))
                    if universal_tag in candidates:
                        candidates.remove(universal_tag)
                    tags = random.sample(candidates, num_tags)
            
            # 写入文件: 1,2,3
            f.write(",".join(map(str, tags)) + "\n")
            
    print(f"Saved labels to {label_path}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--num", type=int, default=10000, help="Number of vectors")
    parser.add_argument("--dim", type=int, default=128, help="Dimension")
    parser.add_argument("--vec_out", type=str, default="vec_data.bin")
    parser.add_argument("--label_out", type=str, default="labels.txt")
    args = parser.parse_args()

    generate_data(args.num, args.dim, args.vec_out, args.label_out)