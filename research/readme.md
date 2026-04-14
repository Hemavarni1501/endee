# 🔍 FAQ Research Assistant using Semantic Search

A lightweight NLP-based research assistant that retrieves the most relevant answers from a predefined FAQ dataset using **sentence embeddings and semantic similarity**.

This project demonstrates how modern **transformer-based embeddings** can be used to build an intelligent question-answering system without relying on heavy backend infrastructure.

---

## 🚀 Features

* 🔎 Semantic search using Sentence Transformers
* 🧠 Context-aware question matching (not keyword-based)
* ⚡ Fast and lightweight — no external database required
* 📂 Works entirely offline after initial model load
* 🖥️ Simple CLI interface for interaction

---

## 🏗️ Architecture

1. **Data Loading**

   * FAQ dataset (`faq.json`) is loaded
   * Questions and answers are extracted

2. **Embedding Generation**

   * Each question is converted into a dense vector using:

     ```
     all-MiniLM-L6-v2
     ```
   * Embeddings are stored in memory

3. **Query Processing**

   * User input is converted into an embedding
   * Cosine similarity is computed against stored vectors

4. **Retrieval**

   * Top-K most similar questions are selected

5. **Response Generation**

   * Best matching answer is returned as final output

---

## 🛠️ Tech Stack

* **Python 3.12**
* **Sentence Transformers**
* **Scikit-learn (Cosine Similarity)**
* **NumPy**

---

## 📁 Project Structure

```
research-assistant/
│── app.py              # Main application
│── faq.json            # Dataset (questions & answers)
│── requirements.txt    # Dependencies
│── README.md           # Project documentation
```

---

## ⚙️ Installation & Setup

### 1. Clone the repository

```
git clone https://github.com/your-username/research-assistant.git
cd research-assistant
```

### 2. Install dependencies

```
pip install -r requirements.txt
```

### 3. Run the application

```
python app.py
```

---

## 💡 Usage

After running the app:

```
Ask a question (or 'exit'):
```

Example:

```
What is machine learning?
```

Output:

* Top matching FAQs
* Similarity scores
* Final selected answer

---

## 🧪 Example

**Input:**

```
What is AI?
```

**Output:**

```
Top Matches:
Q: What is artificial intelligence?
A: Artificial Intelligence is the simulation of human intelligence in machines...
Score: 0.89

Final Answer:
Artificial Intelligence is the simulation of human intelligence in machines...
```

---

## 🎯 Key Highlights

* Eliminates need for traditional keyword search
* Demonstrates practical use of **vector similarity search**
* Fully self-contained (no APIs, no databases)
* Easily extendable to:

  * Chatbot interfaces
  * Web applications (Flask/Streamlit)
  * Large document search systems

---

## 🔮 Future Enhancements

* 🌐 Web UI using Flask or Streamlit
* 🤖 Integration with LLMs for better answer synthesis
* 📚 Support for large document datasets (PDFs, research papers)
* 🗂️ Vector database integration (FAISS, Pinecone)

---

## 👩‍💻 Author

**Hemavarni S**
Computer Science Engineering Student

---

## 📌 Note

This project is built for learning and demonstration purposes, focusing on **semantic search and NLP fundamentals** rather than production-scale deployment.

---
