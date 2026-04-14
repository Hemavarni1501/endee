import json
import numpy as np
from sentence_transformers import SentenceTransformer
from sklearn.metrics.pairwise import cosine_similarity

# Load model
model = SentenceTransformer('all-MiniLM-L6-v2')

# Load FAQ data
with open("faq.json", "r") as f:
    data = json.load(f)

# Prepare embeddings
questions = []
answers = []
embeddings = []

for item in data:
    q = item["question"]
    a = item["answer"]

    questions.append(q)
    answers.append(a)

    emb = model.encode(q)
    embeddings.append(emb)

embeddings = np.array(embeddings)


# 🔍 Semantic search
def search(query, top_k=3):
    query_embedding = model.encode(query).reshape(1, -1)

    similarities = cosine_similarity(query_embedding, embeddings)[0]

    top_indices = np.argsort(similarities)[-top_k:][::-1]

    results = []
    for idx in top_indices:
        results.append({
            "question": questions[idx],
            "answer": answers[idx],
            "score": similarities[idx]
        })

    return results


# 🧠 Simple answer generator
def generate_answer(results):
    # For now: return best answer
    return results[0]["answer"]


# 🚀 CLI
if __name__ == "__main__":
    print("FAQ Research Assistant Ready 🚀")

    while True:
        q = input("\nAsk a question (or 'exit'): ")

        if q.lower() == "exit":
            break

        results = search(q)

        print("\nTop Matches:\n")
        for r in results:
            print(f"Q: {r['question']}")
            print(f"A: {r['answer']}")
            print(f"Score: {r['score']:.4f}")
            print("-" * 50)

        final_answer = generate_answer(results)

        print("\n✅ Final Answer:")
        print(final_answer)