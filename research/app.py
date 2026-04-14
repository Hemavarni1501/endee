import json
import numpy as np
from sentence_transformers import SentenceTransformer

# Load model
model = SentenceTransformer('all-MiniLM-L6-v2')

# Load dataset
with open("faq.json", "r") as f:
    data = json.load(f)

# Prepare vectors
questions = [item["question"] for item in data]
answers = [item["answer"] for item in data]

question_embeddings = model.encode(questions)

# Cosine similarity
def cosine_similarity(a, b):
    return np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b))

def search(query):
    query_embedding = model.encode(query)

    similarities = [
        cosine_similarity(query_embedding, q_emb)
        for q_emb in question_embeddings
    ]

    best_idx = np.argmax(similarities)
    best_score = similarities[best_idx]

    if best_score < 0.5:
        return "Sorry, I couldn't find a relevant answer.", best_score

    return answers[best_idx], best_score


if __name__ == "__main__":
    print("🔍 Research Assistant Ready (type 'exit' to quit)\n")

    while True:
        query = input("You: ")

        if query.lower() == "exit":
            break

        answer, score = search(query)

        print(f"Answer: {answer}")
        print(f"Confidence: {round(score, 2)}\n")