from abc import ABC, abstractmethod


class Encoder(ABC):
    dim: int
    model_name: str

    @abstractmethod
    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        """Return, per input, a list of per-token vectors."""
        raise NotImplementedError


class FakeEncoder(Encoder):
    """Deterministic encoder for contract tests — no model, no GPU.
    Emits one vector per character; role shifts the vectors so doc != query."""

    def __init__(self, dim: int = 4):
        self.dim = dim
        self.model_name = "fake"

    def encode(self, inputs: list[str], role: str) -> list[list[list[float]]]:
        bias = 0.0 if role == "doc" else 0.5
        out = []
        for text in inputs:
            toks = []
            for i, ch in enumerate(text):
                v = [((ord(ch) + i + j) % 7) / 7.0 + bias for j in range(self.dim)]
                toks.append(v)
            if not toks:  # empty input → single zero vector so downstream never sees []
                toks = [[bias] * self.dim]
            out.append(toks)
        return out
