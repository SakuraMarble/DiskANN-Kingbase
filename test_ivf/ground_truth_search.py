import numpy as np
import struct
import argparse
import re
import time
import os

def load_vectors(bin_path):
    # 修复：删除原本的 "wb" open 操作，改用 os.path.exists 检查
    if not os.path.exists(bin_path):
        raise FileNotFoundError(f"Vector file not found: {bin_path}")

    with open(bin_path, "rb") as f:
        # 读取 Header
        header_num = f.read(4)
        if len(header_num) < 4:
            raise ValueError(f"File {bin_path} is empty or corrupted (header missing).")
        num = struct.unpack('I', header_num)[0]
        
        header_dim = f.read(4)
        if len(header_dim) < 4:
            raise ValueError(f"File {bin_path} is corrupted (dim missing).")
        dim = struct.unpack('I', header_dim)[0]
        
        # 读取数据
        # 剩余字节数应该是 num * dim * 4
        expected_bytes = num * dim * 4
        raw_data = f.read()
        
        if len(raw_data) != expected_bytes:
             # 有时可能文件没写完，或者大小不对
             print(f"Warning: Expected {expected_bytes} bytes, got {len(raw_data)} bytes.")
        
        data = np.frombuffer(raw_data, dtype=np.float32)
        
        # 确保 reshape 不会报错
        if data.size != num * dim:
             raise ValueError(f"Data size mismatch. Header says {num}x{dim}={num*dim}, but got {data.size} floats.")
             
        data = data.reshape(num, dim)
        
    return data, num, dim

def load_labels(label_path):
    labels = []
    with open(label_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                labels.append(set())
                continue
            parts = line.split(',')
            tags = set()
            for p in parts:
                if p:
                    tags.add(int(p))
            labels.append(tags)
    return labels

def evaluate_filter(expression, labels, universal_tag=11):
    # 将表达式转换为 Python 可执行的代码
    # 例如: (3&5)|7  ->  ((3 in tags) & (5 in tags)) | (7 in tags)
    # 替换数字 N 为 (N in tags)
    # 注意使用正则替换，避免替换部分数字
    
    def replacer(match):
        tag_id = match.group(0)
        return f"({tag_id} in tags)"
    
    py_expr = re.sub(r'\d+', replacer, expression)
    
    valid_ids = []
    for i, tags in enumerate(labels):
        # 通用标签逻辑：如果包含通用标签，直接通过
        if universal_tag in tags:
            valid_ids.append(i)
            continue
            
        # 否则计算布尔表达式
        try:
            # Python 的 & | 运算符优先级与 C++ 类似，且支持 bool 运算
            if eval(py_expr, {"tags": tags}):
                valid_ids.append(i)
        except Exception as e:
            print(f"Error evaluating expression: {e}")
            return []
            
    return valid_ids

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("vec_path")
    parser.add_argument("label_path")
    parser.add_argument("expression")
    parser.add_argument("top_k", type=int)
    parser.add_argument("output_file")
    args = parser.parse_args()

    # 1. 加载数据
    vectors, num, dim = load_vectors(args.vec_path)
    labels = load_labels(args.label_path)
    
    # 模拟 Query (全 0.1f，与 C++ 代码一致)
    query = np.full(dim, 0.1, dtype=np.float32)

    # 2. 筛选 ID
    start_filter = time.time()
    valid_ids = evaluate_filter(args.expression, labels)
    filter_time = (time.time() - start_filter) * 1000

    print(f"[Python] Filter candidates: {len(valid_ids)}")

    if not valid_ids:
        with open(args.output_file, "w") as f:
            f.write("No candidates found.\n")
        return

    # 3. 计算距离 (Vectorized L2)
    start_search = time.time()
    
    # 只取出符合条件的向量
    target_vectors = vectors[valid_ids]
    
    # 计算 L2 距离: sum((v - q)^2)
    dists = np.sum((target_vectors - query)**2, axis=1)
    
    search_time = (time.time() - start_search) * 1000
    print(f"[Python] Search time: {search_time:.2f} ms")

    # 4. 排序取 Top K
    # argsort 返回的是 target_vectors 里的下标
    sorted_indices_local = np.argsort(dists)
    
    top_k_indices_local = sorted_indices_local[:args.top_k]
    
    results = []
    for idx in top_k_indices_local:
        original_id = valid_ids[idx]
        d = dists[idx]
        results.append((original_id, d))

    # 5. 保存结果 (格式模拟 C++ 输出)
    with open(args.output_file, "w") as f:
        # f.write(f"Top {len(results)} Results:\n")
        for i, (vid, dist) in enumerate(results):
            # 格式: Rank 1: ID=123 Dist=0.456
            # 注意: C++ float 精度和 Python float (double) 打印出来可能微小差异
            # 我们只保留 ID 用于 verify，Dist 仅供参考
            f.write(f"Rank {i+1}: ID={vid} Dist={dist:.6f}\n")

if __name__ == "__main__":
    main()