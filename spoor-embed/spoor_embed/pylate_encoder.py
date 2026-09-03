from spoor_embed.encoder import Encoder


class PyLateEncoder(Encoder):
    """Wraps a ColBERT-class model via PyLate. Imported lazily so the base
    package (and its contract tests) need neither torch nor pylate installed."""

    def __init__(self, model_name: str = "lightonai/GTE-ModernColBERT-v1", device: str | None = None):
        from pylate import models  # lazy: only when the real encoder is constructed
        self.model_name = model_name
        self._model = models.ColBERT(model_name_or_path=model_name, device=device)
        # PyLate ColBERT exposes the per-token embedding dim via the underlying transformer.
        self.dim = int(self._model.get_sentence_embedding_dimension())

    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        is_query = role == "query"
        # PyLate returns one (num_tokens, dim) array per input.
        embeddings = self._model.encode(
            inputs,
            is_query=is_query,
            convert_to_numpy=True,
            show_progress_bar=False,
        )
        return [[[float(x) for x in tok] for tok in doc] for doc in embeddings]
